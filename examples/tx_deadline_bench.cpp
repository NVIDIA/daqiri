/* SPDX-License-Identifier: Apache-2.0 */
#include "tx_deadline_gpu.h"
#include "tx_deadline_gdr.h"
#ifdef DAQIRI_BENCH_GDRCOPY
#include "tx_deadline_gdr_map.h"
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/ptp_clock.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <daqiri/daqiri.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using tx_deadline::Backend;
using tx_deadline::Control;
using tx_deadline::Notify;
using Clock = std::chrono::steady_clock;

void check(daqiri::Status s, const char* action) {
  if (s != daqiri::Status::SUCCESS)
    throw std::runtime_error(std::string(action) + " DAQIRI status=" + std::to_string(int(s)));
}
void cu(cudaError_t s, const char* action) {
  if (s != cudaSuccess)
    throw std::runtime_error(std::string(action) + ": " + cudaGetErrorString(s));
}
uint64_t now_ns() {
  timespec ts{};
  if (clock_gettime(CLOCK_REALTIME, &ts)) throw std::runtime_error("CLOCK_REALTIME read failed");
  return uint64_t(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}
uint64_t be64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
  return value;
}
uint32_t acquire(uint32_t* p) {
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
void release(uint32_t* p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

struct Calibration {
  uint64_t system_ns = 0;
  int64_t offset_ns = 0;
  uint64_t bracket_ns = UINT64_MAX;
};

Calibration calibrate(const std::string& device, uint64_t max_offset, uint64_t max_bracket) {
  const int fd = open(device.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot open " + device);
  ptp_sys_offset_extended data{};
  data.n_samples = PTP_MAX_SAMPLES;
  const int rc = ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &data);
  close(fd);
  if (rc) throw std::runtime_error("PTP_SYS_OFFSET_EXTENDED failed for " + device);
  const auto ns = [](const ptp_clock_time& ts) { return int64_t(ts.sec) * 1000000000LL + ts.nsec; };
  Calibration best;
  for (unsigned i = 0; i < data.n_samples; ++i) {
    const int64_t a = ns(data.ts[i][0]), b = ns(data.ts[i][2]);
    if (b < a) throw std::runtime_error("system clock stepped during calibration");
    const auto bracket = uint64_t(b - a);
    if (bracket < best.bracket_ns)
      best = {uint64_t(a + (b - a) / 2), ns(data.ts[i][1]) - (a + (b - a) / 2), bracket};
  }
  std::cerr << device << " PHC-system=" << best.offset_ns << "ns bracket=" << best.bracket_ns
            << "ns\n";
  if (uint64_t(std::abs(best.offset_ns)) > max_offset || best.bracket_ns > max_bracket)
    throw std::runtime_error("clock synchronization/uncertainty exceeds configured bounds for " +
                             device);
  return best;
}

int64_t offset_at(Calibration a, Calibration b, uint64_t time) {
  if (b.system_ns <= a.system_ns) throw std::runtime_error("system clock did not advance");
  const auto elapsed = std::min(std::max(time, a.system_ns), b.system_ns) - a.system_ns;
  return a.offset_ns +
         int64_t((__int128(b.offset_ns - a.offset_ns) * elapsed) / (b.system_ns - a.system_ns));
}

struct Method {
  std::string name;
  Notify notify;
  bool persistent = false;
  bool gpu_tx = false;
  bool prepared_window = false;
  Backend backend = Backend::Cpu;
};

std::vector<Method> methods() {
  std::vector<Method> result = {{"daqiri-mapped-flag", Notify::MappedFlag},
                                {"daqiri-event-poll", Notify::EventPoll},
                                {"daqiri-stream-poll", Notify::StreamPoll},
                                {"daqiri-event-sync", Notify::EventSync},
                                {"daqiri-host-callback", Notify::HostCallback},
                                {"daqiri-persistent-flag", Notify::MappedFlag, true}};
#ifdef DAQIRI_BENCH_GDRCOPY
  result.push_back({"daqiri-gdrcopy-prebuilt", Notify::MappedFlag, true, true, true, Backend::Gdr});
#endif
#ifdef DAQIRI_BENCH_GPUNETIO
  result.push_back({"gpunetio", Notify::MappedFlag, false, true, false, Backend::Gpunetio});
  result.push_back(
      {"gpunetio-persistent", Notify::MappedFlag, true, true, false, Backend::Gpunetio});
  result.push_back({"gpunetio-prebuilt-persistent", Notify::MappedFlag, true, true, true,
                    Backend::GpunetioPrebuilt});
#endif
  return result;
}

struct Options {
  std::string config, csv = "tx-deadline.csv", method = "all";
  uint32_t samples = 10000, warmup = 1000, repeats = 3, seed = 20261111;
  uint64_t timeout_ms = 100, interval_us = 100;
  uint32_t prepare_window = 32;
  std::vector<uint64_t> leads{2000, 5000, 10000, 20000, 50000, 100000};
  bool notification_only = false;
};

uint64_t number(const std::string& value) {
  size_t used = 0;
  if (value.empty() || value.front() == '-')
    throw std::runtime_error("expected nonnegative integer");
  const auto n = std::stoull(value, &used);
  if (used != value.size()) throw std::runtime_error("invalid integer: " + value);
  return n;
}
uint32_t count(const std::string& value) {
  const auto n = number(value);
  if (n > UINT32_MAX) throw std::runtime_error("count exceeds uint32 range");
  return uint32_t(n);
}
Options options(int argc, char** argv) {
  const std::string usage =
      "usage: daqiri_bench_tx_deadline CONFIG --csv PATH [--method all|NAME[,NAME...]] "
      "[--samples N] [--warmup N] [--repeats N] [--lead-ns 2000,5000,...] "
      "[--timeout-ms N] [--interval-us N] [--prepare-window N] [--seed N] [--notification-only]";
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << usage << '\n';
    std::exit(0);
  }
  if (argc < 2) throw std::runtime_error(usage);
  Options o;
  o.config = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--notification-only") {
      o.notification_only = true;
      continue;
    }
    if (++i == argc) throw std::runtime_error("missing value for " + arg);
    const std::string value = argv[i];
    if (arg == "--csv")
      o.csv = value;
    else if (arg == "--method")
      o.method = value;
    else if (arg == "--samples")
      o.samples = count(value);
    else if (arg == "--warmup")
      o.warmup = count(value);
    else if (arg == "--repeats")
      o.repeats = count(value);
    else if (arg == "--seed")
      o.seed = count(value);
    else if (arg == "--timeout-ms")
      o.timeout_ms = number(value);
    else if (arg == "--interval-us")
      o.interval_us = number(value);
    else if (arg == "--prepare-window")
      o.prepare_window = count(value);
    else if (arg == "--lead-ns") {
      o.leads.clear();
      size_t begin = 0;
      do {
        const auto end = value.find(',', begin);
        o.leads.push_back(number(value.substr(begin, end - begin)));
        if (end == std::string::npos) break;
        begin = end + 1;
      } while (true);
    } else
      throw std::runtime_error("unknown option: " + arg);
  }
  if (!o.samples || !o.repeats || !o.timeout_ms || o.timeout_ms > 1000 ||
      uint64_t(o.samples) + o.warmup >= UINT32_MAX || o.interval_us > 1000000 ||
      std::any_of(o.leads.begin(), o.leads.end(), [](uint64_t n) { return n > 100000000; }))
    throw std::runtime_error(
        "invalid sample count, interval, timeout (1..1000ms), or lead (0..100ms)");
  if (*std::max_element(o.leads.begin(), o.leads.end()) >= o.timeout_ms * 1000000)
    throw std::runtime_error("timeout must exceed every lead time");
  if (!o.prepare_window || o.prepare_window > 32 ||
      uint64_t(o.prepare_window) * o.interval_us * 1000 +
              *std::max_element(o.leads.begin(), o.leads.end()) >=
          3000000000ULL)
    throw std::runtime_error("prepare window must be 1..32 and its schedule less than 3 seconds");
  auto leads = o.leads;
  std::sort(leads.begin(), leads.end());
  if (std::adjacent_find(leads.begin(), leads.end()) != leads.end())
    throw std::runtime_error("duplicate lead time");
  if (o.method != "all") {
    const auto known = methods();
    size_t begin = 0;
    do {
      const auto end = o.method.find(',', begin);
      const auto name = o.method.substr(begin, end - begin);
      if (std::none_of(known.begin(), known.end(), [&](const Method& m) { return m.name == name; }))
        throw std::runtime_error(
            "unknown or unavailable method (GDRCopy/GPUNetIO require their build options): " +
            name);
      if (end == std::string::npos) break;
      begin = end + 1;
    } while (true);
  }
  return o;
}

struct Config {
  std::string tx_name, rx_name, tx_bdf, tx_phc, rx_phc;
  int tx_q = 0, rx_q = 0, gpu = 0, cpu = -1;
  uint64_t max_offset = 1000, max_bracket = 5000;
  uint32_t tx_slots = 0, tx_stride = 0;
  tx_deadline::PacketSpec packet;
};

Config config(const YAML::Node& root) {
  const auto n = root["bench_tx_deadline"];
  if (!n) throw std::runtime_error("missing bench_tx_deadline map");
  Config c;
  c.tx_name = n["tx_interface_name"].as<std::string>();
  c.rx_name = n["rx_interface_name"].as<std::string>();
  c.tx_q = n["tx_queue_id"].as<int>(0);
  c.rx_q = n["rx_queue_id"].as<int>(0);
  c.tx_phc = n["tx_ptp_device"].as<std::string>();
  c.rx_phc = n["rx_ptp_device"].as<std::string>();
  c.cpu = n["cpu_core"].as<int>();
  c.gpu = n["gpu"].as<int>(0);
  c.packet.bytes = n["packet_size"].as<uint32_t>(256);
  c.max_offset = n["max_clock_offset_ns"].as<uint64_t>(c.max_offset);
  c.max_bracket = n["max_clock_bracket_ns"].as<uint64_t>(c.max_bracket);
  if (c.packet.bytes < tx_deadline::kMinBytes || c.packet.bytes > 8192 || c.cpu < 0 ||
      c.cpu >= CPU_SETSIZE)
    throw std::runtime_error("packet_size must be 66..8192 and cpu_core must be valid");
  const auto mac = [&](const char* key, uint8_t* dst) {
    const auto value = n[key].as<std::string>();
    unsigned bytes[6];
    int end = 0;
    if (std::sscanf(value.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%n", &bytes[0], &bytes[1], &bytes[2],
                    &bytes[3], &bytes[4], &bytes[5], &end) != 6 ||
        size_t(end) != value.size())
      throw std::runtime_error("invalid MAC: " + value);
    for (int i = 0; i < 6; ++i) dst[i] = bytes[i];
  };
  mac("eth_src_addr", c.packet.src_mac);
  mac("eth_dst_addr", c.packet.dst_mac);
  for (auto pair : {std::make_pair("ip_src_addr", c.packet.src_ip),
                    std::make_pair("ip_dst_addr", c.packet.dst_ip)})
    if (inet_pton(AF_INET, n[pair.first].as<std::string>().c_str(), pair.second) != 1)
      throw std::runtime_error("invalid IPv4 address");
  c.packet.src_port = n["udp_src_port"].as<uint16_t>(4096);
  c.packet.dst_port = n["udp_dst_port"].as<uint16_t>(4096);
  const auto cfg = root["daqiri"]["cfg"];
  if (cfg["stream_type"].as<std::string>() != "raw" || cfg["engine"].as<std::string>() != "ibverbs")
    throw std::runtime_error("requires raw ibverbs");
  if (!cfg["loopback"].as<std::string>("").empty())
    throw std::runtime_error("requires a cabled path");
  bool tx_found = false, rx_found = false;
  for (auto intf : cfg["interfaces"]) {
    const auto name = intf["name"].as<std::string>();
    if (name == c.tx_name) {
      c.tx_bdf = intf["address"].as<std::string>();
      if (!intf["tx"]["accurate_send"].as<bool>(false))
        throw std::runtime_error("enable tx.accurate_send");
      for (auto q : intf["tx"]["queues"])
        if (q["id"].as<int>() == c.tx_q) {
          tx_found = true;
          if (q["poll_mode"].as<std::string>() != "direct" || q["memory_regions"].size() != 1 ||
              q["offloads"])
            throw std::runtime_error("TX requires direct, one GPU MR, and no header offloads");
          const auto mr_name = q["memory_regions"][0].as<std::string>();
          bool valid = false;
          for (auto mr : cfg["memory_regions"])
            if (mr["name"].as<std::string>() == mr_name) {
              c.tx_slots = mr["num_bufs"].as<uint32_t>();
              c.tx_stride = mr["buf_size"].as<uint32_t>();
              valid = mr["kind"].as<std::string>() == "device" &&
                      mr["affinity"].as<int>() == c.gpu &&
                      mr["buf_size"].as<uint32_t>() >= c.packet.bytes;
              uint64_t stride = 1;
              while (stride < c.tx_stride) stride <<= 1;
              if (stride > UINT32_MAX) throw std::runtime_error("TX slot stride overflows");
              c.tx_stride = uint32_t(stride);
            }
          if (!valid)
            throw std::runtime_error(
                "TX MR must be device memory on bench GPU with sufficient buf_size");
        }
    }
    if (name == c.rx_name) {
      if (!intf["rx"]["hardware_timestamps"].as<bool>(false))
        throw std::runtime_error("enable RX hardware timestamps");
      for (auto q : intf["rx"]["queues"])
        if (q["id"].as<int>() == c.rx_q) {
          rx_found = true;
          if (q["poll_mode"].as<std::string>() != "direct" || q["cqe_compression"].as<bool>(false))
            throw std::runtime_error("RX requires direct polling and uncompressed CQEs");
        }
    }
  }
  if (!tx_found || !rx_found) throw std::runtime_error("benchmark interface/queue not found");
  return c;
}

struct Row {
  uint64_t sequence = 0, launch = 0, ready = 0, submit = 0, deadline = 0, tx = 0, rx = 0;
  uint64_t gpu_start = 0, gpu_ready = 0, gpu_submit = 0;
  uint64_t prepared = 0, command = 0;
  uint64_t first_doorbell = 0;
  std::string status = "ok";
};

class Producer {
 public:
  Control *host = nullptr, *device = nullptr;
  cudaStream_t stream = nullptr;
  cudaEvent_t event = nullptr;
  std::atomic<bool> callback_ready{false};
  bool persistent;
  Method method;
  Config cfg;
  uint32_t token = 0;
  void* queue;
  uint32_t mkey;
  bool kernel_started = false;
  daqiri::GpuTxQueue cached_queue{};
  daqiri::GpuTxTicket* tickets = nullptr;
  uint32_t ticket_capacity = 0;
  void* ticket_allocation = nullptr;
#ifdef DAQIRI_BENCH_GDRCOPY
  std::unique_ptr<GdrMap> ticket_mapping;
#endif

  Producer(Method m, Config c, void* q, uint32_t key)
      : persistent(m.persistent), method(m), cfg(c), queue(q), mkey(key) {
    cu(cudaHostAlloc(reinterpret_cast<void**>(&host), sizeof(Control), cudaHostAllocMapped),
       "mapped control");
    new (host) Control();
    cu(cudaHostGetDevicePointer(reinterpret_cast<void**>(&device), host, 0), "control GPU address");
    cu(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "producer stream");
    cu(cudaEventCreateWithFlags(&event, cudaEventDisableTiming | cudaEventBlockingSync),
       "ready event");
    if (persistent && !method.prepared_window) start_kernel();
  }
  void start_kernel() {
    cu(tx_deadline::launch(device, cfg.packet, persistent, queue, mkey, stream, method.backend,
                           cached_queue, tickets),
       "producer kernel");
    kernel_started = true;
  }
  void init_window(uint32_t capacity) {
    if (!method.prepared_window || !capacity || capacity > 32)
      throw std::runtime_error("invalid ticket capacity");
    ticket_capacity = capacity;
#ifdef DAQIRI_BENCH_GDRCOPY
    if (method.backend == Backend::Gdr) {
      cu(cudaMalloc(&ticket_allocation, 2 * 65536), "allocate GDR tickets");
      tickets = reinterpret_cast<daqiri::GpuTxTicket*>(
          (reinterpret_cast<uint64_t>(ticket_allocation) + 65535) & ~uint64_t(65535));
      ticket_mapping = std::make_unique<GdrMap>();
      ticket_mapping->map(tickets, 65536);
      return;
    }
#endif
    // GPUNetIO's GPU prebuild kernel writes these tickets directly.
    cu(cudaMalloc(reinterpret_cast<void**>(&tickets), capacity * sizeof(*tickets)),
       "allocate SDK GPU tickets");
  }
  void cache_window(daqiri::GpuTxQueue handle, const std::vector<daqiri::GpuTxTicket>& window) {
#ifdef DAQIRI_BENCH_GDRCOPY
    if (!ticket_mapping || window.empty() || window.size() > ticket_capacity)
      throw std::runtime_error("invalid GDR ticket window");
    if (kernel_started &&
        (handle.doorbell != cached_queue.doorbell || handle.record != cached_queue.record))
      throw std::runtime_error("GDR queue handle changed");
    cached_queue = handle;
    ticket_mapping->copy(0, window.data(), window.size() * sizeof(*tickets));
    GdrMap::store_barrier();
    // CPU CUDA ordering API after the BAR writes, before command publication.
    // No CUDA data copy: both WQEs and tickets travel through GDRCopy.
    cu(cudaStreamSynchronize(nullptr), "order GDR WQEs/tickets before commands");
#else
    throw std::runtime_error("build with DAQIRI_BENCH_GDRCOPY=ON");
#endif
  }
  void window_ready() {
    if (persistent && !kernel_started) start_kernel();
  }
  ~Producer() {
    if (persistent && kernel_started) release(&host->request, UINT32_MAX);
    cudaStreamSynchronize(stream);
    cudaEventDestroy(event);
    cudaStreamDestroy(stream);
    cudaFreeHost(host);
#ifdef DAQIRI_BENCH_GDRCOPY
    ticket_mapping.reset();
#endif
    if (ticket_allocation)
      cudaFree(ticket_allocation);
    else
      cudaFree(tickets);
  }
  void start(uint8_t* packet, uint64_t sequence, uint64_t deadline) {
    host->error = 0;
    host->tx_raw_timestamp = 0;
    host->gpu_submit_ns = 0;
    host->first_doorbell_ns = 0;
    host->packet = packet;
    host->sequence = sequence;
    host->deadline_ns = deadline;
    callback_ready.store(false, std::memory_order_relaxed);
    release(&host->request, ++token);
    if (!persistent) {
      start_kernel();
      if (method.notify == Notify::EventPoll || method.notify == Notify::EventSync)
        cu(cudaEventRecord(event, stream), "record ready event");
      if (method.notify == Notify::HostCallback)
        cu(cudaLaunchHostFunc(
               stream,
               [](void* p) {
                 static_cast<Producer*>(p)->callback_ready.store(true, std::memory_order_release);
               },
               this),
           "ready callback");
    }
  }
  bool ready() {
    cudaError_t s = cudaSuccess;
    switch (method.notify) {
      case Notify::MappedFlag:
        return acquire(&host->ready) == token;
      case Notify::HostCallback:
        return callback_ready.load(std::memory_order_acquire);
      case Notify::EventPoll:
        s = cudaEventQuery(event);
        break;
      case Notify::StreamPoll:
        s = cudaStreamQuery(stream);
        break;
      case Notify::EventSync:
        s = cudaEventSynchronize(event);
        break;
    }
    if (s == cudaErrorNotReady) return false;
    cu(s, "GPU readiness");
    return true;
  }
  void finish() {
    // Wait before reusing a command/control block or invoking a new callback.
    const auto until = Clock::now() + std::chrono::seconds(2);
    while (acquire(&host->done) != token) {
      if (Clock::now() > until) throw std::runtime_error("GPU producer did not finish");
    }
    if (!persistent) cu(cudaStreamSynchronize(stream), "finish producer");
    if (host->error)
      throw std::runtime_error("GPU completion error/timeout=" + std::to_string(host->error));
  }
};

void notification_only(const Options& o, const Config& cfg, std::vector<Method> selected) {
  std::ofstream output(o.csv);
  if (!output) throw std::runtime_error("cannot open output CSV");
  output << "method,repeat,sequence,launch_to_ready_ns,gpu_build_ns\n";
  uint8_t* packet = nullptr;
  cu(cudaMalloc(reinterpret_cast<void**>(&packet), cfg.packet.bytes), "notification test packet");
  std::mt19937 random(o.seed);
  for (uint32_t repeat = 0; repeat < o.repeats; ++repeat) {
    std::shuffle(selected.begin(), selected.end(), random);
    for (const auto& method : selected) {
      Producer producer(method, cfg, nullptr, 0);
      std::vector<uint64_t> times;
      for (uint64_t i = 0; i < uint64_t(o.samples) + o.warmup; ++i) {
        const auto start = Clock::now();
        producer.start(packet, i + 1, 123456789);
        const auto until = start + std::chrono::milliseconds(o.timeout_ms);
        while (!producer.ready()) {
          if (Clock::now() > until) throw std::runtime_error("notification-only timeout");
        }
        const auto seen = Clock::now();
        producer.finish();
        if (i == 0) {
          std::vector<uint8_t> bytes(cfg.packet.bytes);
          cu(cudaMemcpy(bytes.data(), packet, bytes.size(), cudaMemcpyDeviceToHost),
             "verify GPU packet");
          uint32_t sum = 0;
          for (int j = 14; j < 34; j += 2) sum += (uint32_t(bytes[j]) << 8) | bytes[j + 1];
          while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
          if (sum != 0xffff || be64(bytes.data() + 46) != i + 1 ||
              be64(bytes.data() + 54) != 123456789 ||
              std::memcmp(bytes.data(), cfg.packet.dst_mac, 6) ||
              std::memcmp(bytes.data() + 6, cfg.packet.src_mac, 6))
            throw std::runtime_error("GPU packet verification failed");
        }
        if (i >= o.warmup) {
          const auto ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(seen - start).count();
          times.push_back(ns);
          output << method.name << ',' << repeat << ',' << i + 1 << ',' << ns << ','
                 << producer.host->gpu_ready_ns - producer.host->gpu_start_ns << '\n';
        }
        while (Clock::now() < start + std::chrono::microseconds(o.interval_us)) {
        }
      }
      std::sort(times.begin(), times.end());
      const auto p = [&](double q) {
        return times[std::min(times.size() - 1, size_t(std::ceil(times.size() * q)) - 1)];
      };
      std::cout << method.name << " repeat=" << repeat << " launch_to_ready_ns p50=" << p(0.5)
                << " p99=" << p(0.99) << " p999=" << p(0.999) << '\n';
      output.flush();
    }
  }
  cu(cudaFree(packet), "free notification packet");
  if (!output) throw std::runtime_error("CSV write failed");
}

struct PreparedPacket {
  uint8_t* packet = nullptr;
  daqiri::TxTimestampId id = daqiri::INVALID_TX_TIMESTAMP_ID;
  daqiri::GpuTxSubmission submission{};
  uint64_t sequence = 0, epoch = 0, deadline = 0, prepared = 0;
};

Row sample(Producer& producer, const Config& cfg, int tx_port, int rx_port, uint64_t seq,
           uint64_t lead, uint64_t timeout_ms, uint64_t epoch = 0,
           const PreparedPacket* prepared = nullptr, uint32_t ticket_index = 0) {
  Row r;
  r.sequence = seq;
  daqiri::BurstParams* tx = nullptr;
  daqiri::TxTimestampId id = daqiri::INVALID_TX_TIMESTAMP_ID;
  uint8_t* packet = nullptr;
  if (prepared) {
    packet = prepared->packet;
    id = prepared->id;
    r.prepared = prepared->prepared;
    if (seq != prepared->sequence || epoch + lead != prepared->deadline)
      throw std::runtime_error("prepared command/timestamp mismatch");
  }
#ifdef DAQIRI_BENCH_GPUNETIO
  // Caller supplies the GPUNetIO packet pointer via the control block.
  if (producer.method.gpu_tx && !prepared) packet = producer.host->packet;
#endif
  if (!producer.method.gpu_tx && !prepared) {
    tx = daqiri::create_tx_burst_params();
    if (!tx) throw std::runtime_error("no TX metadata");
    daqiri::set_header(tx, tx_port, cfg.tx_q, 1, 1);
    check(daqiri::get_tx_packet_burst(tx), "allocate direct TX packet");
    packet = static_cast<uint8_t*>(daqiri::get_packet_ptr(tx, 0));
    check(daqiri::set_packet_lengths(tx, 0, {int(cfg.packet.bytes)}), "set packet size");
    check(daqiri::request_packet_tx_timestamp(tx, 0, &id), "request TX completion time");
  }
  // A fixed command schedule includes late CPU dispatch in every method's
  // budget. Prepared WQEs/tickets are constructed before that schedule begins.
  while (epoch && now_ns() < epoch) {
  }
  r.launch = epoch ? epoch : now_ns();
  r.deadline = r.launch + lead;
  r.command = now_ns();
  producer.host->ticket_index = ticket_index;
  producer.start(packet, seq, r.deadline);
  const auto until = Clock::now() + std::chrono::milliseconds(timeout_ms);
  if (!producer.method.gpu_tx) {
    bool ready = false;
    while (Clock::now() < until && !(ready = producer.ready())) {
    }
    if (!ready) {
      r.status = "notify_timeout";
      producer.finish();
      daqiri::free_all_packets_and_burst_tx(tx);
      daqiri::release_tx_timestamp(id);
      return r;
    }
    r.ready = now_ns();
    check(daqiri::set_packet_tx_time(tx, 0, r.deadline), "set deadline");
    const auto sent = daqiri::send_tx_burst(tx);
    r.submit = now_ns();
    if (sent != daqiri::Status::SUCCESS) {
      if (sent != daqiri::Status::NO_SPACE_AVAILABLE) daqiri::free_all_packets_and_burst_tx(tx);
      daqiri::release_tx_timestamp(id);
      producer.finish();
      r.status = "send_failed";
      return r;
    }
  }
  while (Clock::now() < until && (!r.tx || !r.rx)) {
    if (!r.tx && producer.method.gpu_tx && acquire(&producer.host->done) == producer.token) {
      if (producer.host->error)
        throw std::runtime_error("GPU TX failed, GPU error=" +
                                 std::to_string(producer.host->error));
      const auto raw = producer.host->tx_raw_timestamp;
      // Accurate-send CQs use packed UTC. Reject other clock formats.
      if ((raw & UINT32_MAX) >= 1000000000)
        throw std::runtime_error("GPU TX CQ has no packed UTC timestamp");
      r.tx = (raw >> 32) * 1000000000ULL + (raw & UINT32_MAX);
      if (r.tx < r.deadline - 1000000000ULL || r.tx > r.deadline + 1000000000ULL)
        throw std::runtime_error("GPU TX CQ timestamp clock does not match the synchronized PHC");
    }
    if (!r.tx && !producer.method.gpu_tx) {
      const auto s = daqiri::get_packet_tx_timestamp(id, &r.tx);
      if (s != daqiri::Status::NOT_READY) check(s, "TX completion");
    }
    daqiri::BurstParams* rx = nullptr;
    const auto s = daqiri::get_rx_burst(&rx, rx_port, cfg.rx_q);
    if (s == daqiri::Status::SUCCESS && rx) {
      for (uint32_t i = 0; i < daqiri::get_num_packets(rx); ++i) {
        if (daqiri::get_packet_length(rx, i) < tx_deadline::kMinBytes) continue;
        uint8_t marker[24];
        cu(cudaMemcpy(marker, static_cast<uint8_t*>(daqiri::get_packet_ptr(rx, i)) + 42,
                      sizeof(marker), cudaMemcpyDefault),
           "read RX marker");
        const uint32_t magic = (uint32_t(marker[0]) << 24) | (uint32_t(marker[1]) << 16) |
                               (uint32_t(marker[2]) << 8) | marker[3];
        if (magic != tx_deadline::kMagic || be64(marker + 4) != seq) continue;
        if (be64(marker + 12) != r.deadline || marker[20] != 0x12 || marker[21] != 0x34 ||
            marker[22] != 0x56 || marker[23] != 0x78) {
          daqiri::free_all_packets_and_burst_rx(rx);
          throw std::runtime_error("received marker corruption");
        }
        check(daqiri::get_packet_rx_timestamp(rx, i, &r.rx), "RX hardware timestamp");
      }
      daqiri::free_all_packets_and_burst_rx(rx);
    } else if (s != daqiri::Status::NOT_READY && s != daqiri::Status::SUCCESS) {
      check(s, "RX poll");
    }
  }
  producer.finish();
  r.gpu_start = producer.host->gpu_start_ns;
  r.gpu_ready = producer.host->gpu_ready_ns;
  r.gpu_submit = producer.host->gpu_submit_ns;
  r.first_doorbell = producer.host->first_doorbell_ns;
  if (id != daqiri::INVALID_TX_TIMESTAMP_ID)
    check(daqiri::release_tx_timestamp(id), "release TX timestamp");
  if (!r.tx || !r.rx)
    r.status = !r.tx && !r.rx ? "tx_rx_timeout" : !r.tx ? "tx_timeout" : "rx_timeout";
  return r;
}
}  // namespace

void separate_helpers() {
  const auto* value = std::getenv("DAQIRI_TX_HELPER_CPU");
  if (!value) return;
  const int cpu = std::stoi(value);
  if (cpu < 0 || cpu >= CPU_SETSIZE) throw std::runtime_error("invalid helper CPU");
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  const int main_tid = getpid();
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
    const int tid = std::stoi(entry.path().filename());
    if (tid == main_tid) continue;
    if (sched_setaffinity(tid, sizeof(mask), &mask) != 0 && errno != ESRCH)
      throw std::runtime_error("cannot separate helper CPU affinity");
  }
}
int main(int argc, char** argv) {
  bool initialized = false;
  try {
    const auto o = options(argc, argv);
    const auto cfg = config(YAML::LoadFile(o.config));
    auto selected = methods();
    selected.erase(std::remove_if(selected.begin(), selected.end(),
                                  [&](const Method& m) {
                                    return (o.method != "all" &&
                                            ("," + o.method + ",").find("," + m.name + ",") ==
                                                std::string::npos) ||
                                           (o.notification_only && (m.gpu_tx));
                                  }),
                   selected.end());
    if (selected.empty())
      throw std::runtime_error(
          "unknown or unavailable method (GDRCopy/GPUNetIO require their build options)");
    const bool scheduled = std::any_of(selected.begin(), selected.end(),
                                       [](const Method& m) { return m.prepared_window; });
    if (scheduled && (!o.interval_us || o.prepare_window > cfg.tx_slots))
      throw std::runtime_error("prepared windows require an interval and sufficient TX slots");
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cfg.cpu, &mask);
    if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask))
      throw std::runtime_error("cannot pin benchmark CPU");
    cu(cudaSetDevice(cfg.gpu), "select GPU");
    cudaDeviceProp prop{};
    cu(cudaGetDeviceProperties(&prop, cfg.gpu), "GPU properties");
    if (prop.major < 8 || !prop.canMapHostMemory)
      throw std::runtime_error("requires Ampere or newer with mapped host memory");
    std::cerr << "GPU=" << prop.name << " mapped=" << prop.canMapHostMemory << " CPU=" << cfg.cpu
              << '\n';
    if (o.notification_only) {
      notification_only(o, cfg, selected);
      return 0;
    }
    std::ofstream output(o.csv);
    if (!output) throw std::runtime_error("cannot open output CSV");
    output << "method,repeat,seed,packet_size,lead_ns,sequence,launch_ns,cpu_ready_ns,cpu_submit_"
              "ns,gpu_start_ns,gpu_ready_ns,gpu_submit_ns,deadline_ns,tx_completion_ns,rx_hw_ns,tx_"
              "clock_offset_ns,rx_clock_offset_ns,clock_uncertainty_ns,status,expected_methods,"
              "expected_leads,expected_repeats,expected_samples,cpu_prepared_ns,gpu_command_ns,"
              "gpu_first_doorbell_ns,scheduled_commands,prepared_window\n";
    calibrate(cfg.tx_phc, cfg.max_offset, cfg.max_bracket);
    calibrate(cfg.rx_phc, cfg.max_offset, cfg.max_bracket);
    check(daqiri::daqiri_init(o.config), "initialize");
    initialized = true;
    const int tx_port = daqiri::get_port_id(cfg.tx_name),
              rx_port = daqiri::get_port_id(cfg.rx_name);
    if (tx_port < 0 || rx_port < 0) throw std::runtime_error("cannot resolve interface IDs");
    std::mt19937 random(o.seed);
    uint64_t sequence = (now_ns() << 8) | 1;
    for (uint32_t repeat = 0; repeat < o.repeats; ++repeat) {
      auto shuffled = selected;
      std::shuffle(shuffled.begin(), shuffled.end(), random);
      auto leads = o.leads;
      std::shuffle(leads.begin(), leads.end(), random);
      for (const auto& method : shuffled) {
        std::cerr << "repeat=" << repeat << " method=" << method.name << '\n';
#ifdef DAQIRI_BENCH_GPUNETIO
        std::unique_ptr<tx_deadline::Gpunetio> gpu_tx;
        if (method.backend == Backend::Gpunetio || method.backend == Backend::GpunetioPrebuilt)
          gpu_tx = std::make_unique<tx_deadline::Gpunetio>(cfg.tx_bdf.c_str(), cfg.gpu,
                                                           cfg.tx_slots, cfg.tx_stride);
#endif
        void* queue = nullptr;
        uint32_t key = 0;
#ifdef DAQIRI_BENCH_GPUNETIO
        if (gpu_tx) {
          queue = gpu_tx->queue();
          key = gpu_tx->mkey_be();
        }
#endif
#ifdef DAQIRI_BENCH_GDRCOPY
        std::unique_ptr<tx_deadline::GdrTx> gdr;
        if (method.backend == Backend::Gdr) {
          gdr =
              std::make_unique<tx_deadline::GdrTx>(cfg.tx_bdf.c_str(), cfg.tx_slots, cfg.tx_stride);
          queue = gdr->queue();
        }
#endif
        Producer producer(method, cfg, queue, key);
        if (method.prepared_window) producer.init_window(o.prepare_window);
        separate_helpers();
        for (auto lead : leads) {
          const auto tx_before = calibrate(cfg.tx_phc, cfg.max_offset, cfg.max_bracket);
          const auto rx_before = calibrate(cfg.rx_phc, cfg.max_offset, cfg.max_bracket);
          std::vector<Row> rows;
          rows.reserve(o.samples);
          const uint32_t window_size = scheduled ? o.prepare_window : 1;
          const uint64_t count = uint64_t(o.samples) + o.warmup;
          for (uint64_t base = 0; base < count; base += window_size) {
            const uint32_t n = std::min<uint64_t>(window_size, count - base);
            const uint64_t first_epoch = scheduled ? now_ns() + (base ? 500000 : 20000000) : 0;
            std::vector<PreparedPacket> packets;
            if (method.prepared_window) {
              std::vector<daqiri::GpuTxTicket> tickets;
              packets.reserve(n);
              tickets.reserve(n);
              std::vector<tx_deadline::PreparedTx> window;
#ifdef DAQIRI_BENCH_GDRCOPY
              if (gdr)
                window = gdr->prepare_window(n, cfg.packet.bytes, first_epoch, o.interval_us * 1000,
                                             lead);
#endif
#ifdef DAQIRI_BENCH_GPUNETIO
              if (gpu_tx)
                window = gpu_tx->prepare_gpu_window(n, cfg.packet.bytes, first_epoch,
                                                    o.interval_us * 1000, lead, producer.tickets);
#endif
              if (window.size() != n) throw std::runtime_error("missing prepared TX window");
              for (uint32_t j = 0; j < n; ++j) {
                PreparedPacket p;
                p.packet = window[j].packet;
                p.submission = window[j].submission;
                p.sequence = ++sequence;
                p.epoch = first_epoch + uint64_t(j) * o.interval_us * 1000;
                p.deadline = p.epoch + lead;
                p.prepared = now_ns();
                packets.push_back(p);
                tickets.push_back(daqiri::gpu_tx_ticket(p.submission));
              }
              if (method.backend == Backend::Gdr)
                producer.cache_window(daqiri::gpu_tx_queue(packets.front().submission), tickets);
              producer.window_ready();
              const auto published = now_ns();
              for (auto& p : packets) p.prepared = published;
            }
            for (uint32_t j = 0; j < n; ++j) {
              const auto start = Clock::now();
              producer.host->packet = nullptr;
#ifdef DAQIRI_BENCH_GPUNETIO
              if (gpu_tx && !method.prepared_window) producer.host->packet = gpu_tx->packet();
#endif
              const auto* packet = method.prepared_window ? &packets[j] : nullptr;
              auto row = sample(producer, cfg, tx_port, rx_port,
                                packet ? packet->sequence : ++sequence, lead, o.timeout_ms,
                                scheduled ? first_epoch + uint64_t(j) * o.interval_us * 1000 : 0,
                                packet, j);
              if (base + j >= o.warmup) rows.push_back(std::move(row));
              if (!scheduled) {
                const auto next = start + std::chrono::microseconds(o.interval_us);
                while (Clock::now() < next) {
                }
              }
            }
          }
          const auto tx_after = calibrate(cfg.tx_phc, cfg.max_offset, cfg.max_bracket);
          const auto rx_after = calibrate(cfg.rx_phc, cfg.max_offset, cfg.max_bracket);
          const auto uncertainty = (std::max(tx_before.bracket_ns, tx_after.bracket_ns) +
                                    std::max(rx_before.bracket_ns, rx_after.bracket_ns) + 1) /
                                   2;
          for (const auto& r : rows) {
            output << method.name << ',' << repeat << ',' << o.seed << ',' << cfg.packet.bytes
                   << ',' << lead << ',' << r.sequence << ',' << r.launch << ',' << r.ready << ','
                   << r.submit << ',' << r.gpu_start << ',' << r.gpu_ready << ',' << r.gpu_submit
                   << ',' << r.deadline << ',' << r.tx << ',' << r.rx << ','
                   << offset_at(tx_before, tx_after, r.deadline) << ','
                   << offset_at(rx_before, rx_after, r.deadline) << ',' << uncertainty << ','
                   << r.status << ',' << selected.size() << ',' << o.leads.size() << ','
                   << o.repeats << ',' << o.samples << ',' << r.prepared << ',' << r.command << ','
                   << r.first_doorbell << ',' << scheduled << ','
                   << (method.prepared_window ? window_size : 0) << '\n';
          }
          output.flush();
          if (!output) throw std::runtime_error("CSV write failed");
          uint64_t misses = 0, missing = 0;
          for (const auto& r : rows) {
            const auto tx_error =
                int64_t(r.tx) - offset_at(tx_before, tx_after, r.deadline) - int64_t(r.deadline);
            const auto rx_error =
                int64_t(r.rx) - offset_at(rx_before, rx_after, r.deadline) - int64_t(r.deadline);
            missing += !r.tx || !r.rx || r.status != "ok";
            misses += !r.tx || !r.rx || r.status != "ok" || tx_error > 2000 || rx_error > 2200;
          }
          std::cerr << "CONDITION repeat=" << repeat << " method=" << method.name
                    << " lead=" << lead << " samples=" << rows.size() << " misses=" << misses
                    << " missing=" << missing << '\n';
        }
      }
    }
    daqiri::print_stats();
    daqiri::shutdown();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "tx deadline benchmark: " << error.what() << '\n';
    if (initialized) daqiri::shutdown();
    return 1;
  }
}

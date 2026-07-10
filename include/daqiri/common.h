/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <daqiri/logging.hpp>
#include <daqiri/types.h>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdint.h>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace daqiri {

// this part is purely optional, just a helper for the user
BurstParams *create_burst_params();
BurstParams *create_tx_burst_params();

enum class ErrorGlobalStats {
  OUT_OF_RX_BUFFERS = 0,
  RX_QUEUE_FULL = 1,
  METADATA_BUF_DEPLETED = 2,

  SENTINEL = 3,
};

struct FileWriteHandle;

struct FileWriteStatus {
  uint32_t completed_packets = 0;
  uint32_t failed_packets = 0;
  uint64_t bytes_written = 0;
};

struct S3Writer;
struct S3WriteHandle;

struct S3WriterConfig {
  std::string bucket;
  std::string region;
  std::string endpoint_override;
  bool path_style = false;
  bool aws_sdk_already_initialized = false;
  uint32_t max_inflight_uploads = 8;
  uint64_t max_staged_bytes = 1ULL << 30;
};

struct S3WriteStatus {
  uint32_t completed_objects = 0;
  uint32_t failed_objects = 0;
  uint64_t bytes_uploaded = 0;
};

static constexpr uint32_t DEFAULT_TX_META_BUFFERS = 1UL << 8;
static constexpr uint32_t DEFAULT_RX_META_BUFFERS = 1UL << 8;
namespace detail {
inline Direction DirectionStringToType(const std::string &dir) {
  if (dir == "rx") {
    return Direction::RX;
  } else if (dir == "tx") {
    return Direction::TX;
  }

  return Direction::TX_RX;
}
}; // namespace detail

/**
 * @brief Determine which directions are enabled
 *
 * @param dir Direction from config. Either "rx", "tx", or "tx/rx"
 * @return int Number of directions enabled
 */
inline int EnabledDirections(const std::string &dir) {
  if (dir == "rx" || dir == "tx") {
    return 1;
  }

  return 0;
}

/**
 * @brief Initialize the engine and any other resources needed
 *
 * @param config YML Configuration structure (e.g. AdvNetConfigYaml)
 * @return AdvNetStatus indicating status. Valid values are:
 *    SUCCESS: Initialization successful
 *    INVALID_CONFIG: Invalid configuration
 *    INTERNAL_ERROR: Internal error
 */
Status daqiri_init(NetworkConfig &config);
Status daqiri_init(const std::string &yaml_string_or_path);
Status daqiri_init_from_yaml_string(const std::string &yaml_string);
Status daqiri_init_from_yaml_file(const std::string &yaml_path);

Status parse_network_config(const std::string &yaml_string_or_path,
                            NetworkConfig &config);
Status parse_network_config_from_yaml_string(const std::string &yaml_string,
                                             NetworkConfig &config);
Status parse_network_config_from_yaml_file(const std::string &yaml_path,
                                           NetworkConfig &config);

/**
 * @brief Returns an engine type
 *
 * @return Engine type
 */
EngineType get_engine_type();

/**
 * @brief Returns an engine type
 *
 * @param config YML Configuration structure (e.g. NetworkConfig)
 * @return Engine type
 */
template <typename Config> EngineType get_engine_type(const Config &config);

/**
 * @brief Returns a raw packet pointer from a pointer in BurstParams
 *
 * The BurstParams structure contains pointers to opaque packets which are not
 * accessible directly by the user. This function fetches the CPU packet pointer
 * at index idx from the burst.
 *
 * @param burst Burst structure containing packets
 * @param seg Segment of packet
 * @param idx Index of packet
 * @return Pointer to packet data
 */
void *get_segment_packet_ptr(BurstParams *burst, int seg, int idx);

/**
 * @brief Returns a raw packet pointer from a pointer in BurstParams
 *
 * The BurstParams structure contains pointers to opaque packets which are not
 * accessible directly by the user. This function fetches the GPU packet pointer
 * at index idx from the burst.
 *
 * @param burst Burst structure containing packets
 * @param idx Index of packet
 * @return Pointer to packet data
 */
void *get_packet_ptr(BurstParams *burst, int idx);

/**
 * @brief Get packet length of a segment of a packet
 *
 * @param burst Burst structure containing packets
 * @param seg Segment of packet
 * @param idx Index of packet
 * @return uint16_t Length of packet
 */
uint32_t get_segment_packet_length(BurstParams *burst, int seg, int idx);

/**
 * @brief Get packet length of an entire packet
 *
 * @param burst Burst structure containing packets
 * @param idx Index of packet
 * @return uint32_t Length of packet
 */
uint32_t get_packet_length(BurstParams *burst, int idx);

/**
 * @brief Get flow ID of a packet
 *
 * Retrieves the flow ID of a packet, or 0 if no flow was matched. The flow ID
 * should match the flow ID in the flow rule for the daqiri config.
 *
 * @param burst Burst structure containing packets
 * @param idx Index of packet
 * @return FlowId Flow ID, or 0 when no flow matched
 */
FlowId get_packet_flow_id(BurstParams* burst, int idx);

/**
 * @brief Enqueue creation of a dynamic RX flow rule.
 *
 * The flow ID is allocated by DAQIRI and returned in the completion result from
 * poll_flow_op(). Packets matching the dynamic rule are marked with the same
 * ID, so get_packet_flow_id() can be used to identify and later delete the rule.
 *
 * @param port Port ID of interface
 * @param flow Rule match and queue action to install
 * @param op_id Output operation ID used to track completion
 * @return Status indicating whether the operation was accepted
 */
Status add_rx_flow_async(int port, const FlowRuleConfig &flow, FlowOpId *op_id);

/**
 * @brief Enqueue creation of multiple dynamic RX flow rules.
 *
 * The batch add completion is delivered as a single FlowOpResult. On success,
 * FlowOpResult::flow_ids_ contains one FlowId per input rule, in input order.
 * If the batch completion fails, nonzero entries in flow_ids_ were installed
 * and can be deleted with delete_flow_async(); zero entries were not installed.
 *
 * @param port Port ID of interface
 * @param flows Rule matches and queue actions to install
 * @param op_id Output operation ID used to track completion
 * @return Status indicating whether the operation was accepted
 */
Status add_rx_flows_async(int port, const std::vector<FlowRuleConfig> &flows, FlowOpId *op_id);

/**
 * @brief Enqueue deletion of a dynamic flow rule.
 *
 * Only flows created by add_rx_flow_async() or add_rx_flows_async() are
 * deletable through this API. The add operation must have completed
 * successfully before the flow can be deleted.
 *
 * @param flow_id Dynamic flow ID returned by an add completion
 * @param op_id Output operation ID used to track completion
 * @return Status indicating whether the operation was accepted
 */
Status delete_flow_async(FlowId flow_id, FlowOpId *op_id);

/**
 * @brief Poll one dynamic flow operation completion.
 *
 * @param result Output completion details
 * @return SUCCESS when a completion was returned, NOT_READY when none are ready
 */
Status poll_flow_op(FlowOpResult *result);

/**
 * @brief Get the hardware RX timestamp of a packet in nanoseconds
 *
 * Retrieves the 64-bit receive timestamp for a packet when the DPDK engine
 * was configured with rx.hardware_timestamps enabled and the NIC provided a
 * timestamp for this packet. The value is converted to nanoseconds in the NIC
 * timestamp clock domain; it is not converted to wall-clock or PTP time.
 *
 * @param burst Burst structure containing packets
 * @param idx Index of packet
 * @param timestamp_ns Output pointer for the RX timestamp in nanoseconds
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Timestamp retrieved
 *    NULL_PTR: Burst or output pointer is null
 *    INVALID_PARAMETER: Packet index is out of range
 *    NOT_SUPPORTED: Timestamp is unavailable for this packet/engine
 */
Status get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns);

/**
 * @brief Populate a TX packet burst buffer
 *
 * Populates a transmit packet burst buffer with allocated packets. The user can
 * take these allocated packets and fill with the desired data/headers.
 *
 * @param burst Burst structure to populate
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Packets allocated
 *    NULL_PTR: Burst or packet pools uninitialized
 *    NO_FREE_BURST_BUFFERS: No burst buffers to allocate
 *    NO_FREE_CPU_PACKET_BUFFERS: Not enough CPU packet buffers available
 */
Status get_tx_packet_burst(BurstParams *burst);

/**
 * @brief Set IPv4 header in packet
 *
 * @param burst Burst structure to populate
 * @param idx Index of packet
 * @param dst_addr Ethernet destination address
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Packet populated successfully
 */
Status set_eth_header(BurstParams *burst, int idx, char *dst_addr);

/**
 * @brief Set IPv4 header in packet
 *
 * @param burst Burst structure to populate
 * @param idx Index of packet
 * @param ip_len Length of packet after IPv4 header
 * @param proto L4 protocol
 * @param src_host Source host in host byte order
 * @param dst_host Destination host in host byte order
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Packet populated successfully
 */
Status set_ipv4_header(BurstParams *burst, int idx, int ip_len, uint8_t proto,
                       unsigned int src_host, unsigned int dst_host);

/**
 * @brief Set UDP header in packet
 *
 * @param burst Burst structure to populate
 * @param idx Index of packet
 * @param udp_len Length of packet after UDP header
 * @param src_port Source port
 * @param dst_port Destination port
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Packet populated successfully
 */
Status set_udp_header(BurstParams *burst, int idx, int udp_len,
                      uint16_t src_port, uint16_t dst_port);

/**
 * @brief Set UDP payload in packet
 *
 * @param burst Burst structure to populate
 * @param idx Index of packet
 * @param data Payload data after UDP header
 * @param len Length of payload
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Packet populated successfully
 */
Status set_udp_payload(BurstParams *burst, int idx, void *data, int len);

/**
 * @brief Test if a TX burst is available
 *
 * Checks whether a TX burst for a given size can be allocated. This is useful
 * for an application to throttle its transmissions if the NIC is not keeping up
 * with the desired rate. Rather than returning an error, the user can use this
 * function to loop or return later to try again.
 *
 * @param burst Info about burst of packets
 * @return true Burst is available
 * @return false Burst is not available
 */
bool is_tx_burst_available(BurstParams *burst);

/**
 * @brief Free all packets and burst from one segment
 *
 * Frees every allocated packets in the burst and the burst metadata for one
 * segment. After this call completes the segment's pointers are no longer
 * valid.
 *
 * @param burst Burst to free
 */
void free_segment_packets_and_burst(BurstParams *burst, int seg);

/**
 * @brief Free all packets and an RX burst
 *
 * Frees all packets in a burst of packets and the associated burst buffer
 *
 * @param burst Burst structure containing packet lists
 */
void free_all_packets_and_burst_rx(BurstParams *burst);

/**
 * @brief Free all packets and a TX burst
 *
 * Frees all packets in a burst of packets and the associated burst buffer
 *
 * @param burst Burst structure containing packet lists
 */
void free_all_packets_and_burst_tx(BurstParams *burst);

/**
 * @brief Set packet lengths in metadata
 *
 * Sets metadata packet lengths. This is needed in addition to L3+L4 lengths for
 * hardware
 *
 * @param burst Burst structure containing packet lists
 * @param idx Index of packet
 * @param lens Lengths of each segment
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Packet populated successfully
 */
Status set_packet_lengths(BurstParams *burst, int idx,
                          const std::initializer_list<int> &lens);

/**
 * @brief Set the same packet lengths for every packet in a burst
 *
 * Sets metadata packet lengths for all packets in a burst. This is useful for
 * benchmarks and fixed-size packet streams where all packets have the same
 * segment lengths.
 *
 * @param burst Burst structure containing packet lists
 * @param lens Lengths of each segment
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Packet lengths populated successfully
 */
Status set_all_packet_lengths(BurstParams *burst,
                              const std::initializer_list<int> &lens);

/**
 * @brief Set packet TX time
 *
 * Sets the transmit time (in PTP time) to transmit the packet. Every packet
 * transmitted after this one in the same queue will be transmitted no earlier
 * than the time listed in the function call. This feature is only available on
 * ConnectX-7 or BlueField 3 and higher cards.
 *
 * @param burst Burst structure containing packet lists
 * @param idx Index of packet
 * @param time PTP time to transmit
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Time set successfully
 */
Status set_packet_tx_time(BurstParams *burst, int idx, uint64_t time);

uint64_t get_burst_tot_byte(BurstParams *burst);

/**
 * @brief Write each packet in a burst to a separate raw binary file.
 *
 * The output files are named absolute_path/file_prefix_<packet_index>. Existing
 * files are truncated. packet_data_offset skips bytes from the logical packet
 * data before writing, walking across all packet segments in order. Host-backed
 * packet segments are written with POSIX file APIs. CUDA device-backed segments
 * require DAQIRI_ENABLE_GDS=ON and cuFile support.
 *
 * @param burst Burst structure containing packets
 * @param absolute_path Existing absolute directory path to write into
 * @param file_prefix Prefix used for each output file
 * @param packet_data_offset Bytes to skip from the start of each logical packet
 * @return Status indicating success or failure
 */
Status daqiri_write_raw_to_file(BurstParams *burst,
                                const std::string &absolute_path,
                                const std::string &file_prefix,
                                uint64_t packet_data_offset);

/**
 * @brief Asynchronously write each packet in a burst to a separate raw binary
 * file.
 *
 * The caller must keep the burst and packet buffers alive until
 * daqiri_file_write_wait() reports completion or daqiri_file_write_destroy()
 * has completed cleanup. Host-backed segments may complete during submission;
 * CUDA device-backed segments are submitted through cuFile batch I/O.
 *
 * @param burst Burst structure containing packets
 * @param absolute_path Existing absolute directory path to write into
 * @param file_prefix Prefix used for each output file
 * @param packet_data_offset Bytes to skip from the start of each logical packet
 * @param handle Output handle for polling, waiting, and cleanup
 * @return Status indicating whether submission succeeded
 */
Status daqiri_write_raw_to_file_async(BurstParams *burst,
                                      const std::string &absolute_path,
                                      const std::string &file_prefix,
                                      uint64_t packet_data_offset,
                                      FileWriteHandle **handle);

/**
 * @brief Append a burst to a classic pcap file.
 *
 * The output file is named absolute_path/file_prefix.pcap. If the file does not
 * exist or is empty, a classic pcap v2.4 global header is written first. If the
 * file exists, it must already contain a compatible Ethernet,
 * microsecond-resolution pcap header. PCAP writes always include full logical
 * packets across all segments; no packet-data offset is applied.
 *
 * @param burst Burst structure containing packets
 * @param absolute_path Existing absolute directory path to write into
 * @param file_prefix Prefix used for the pcap output file
 * @return Status indicating success or failure
 */
Status daqiri_write_pcap_to_file(BurstParams *burst,
                                 const std::string &absolute_path,
                                 const std::string &file_prefix);

/**
 * @brief Asynchronously append a burst to a classic pcap file.
 *
 * The caller must keep the burst and packet buffers alive until
 * daqiri_file_write_wait() reports completion or daqiri_file_write_destroy()
 * has completed cleanup.
 *
 * @param burst Burst structure containing packets
 * @param absolute_path Existing absolute directory path to write into
 * @param file_prefix Prefix used for the pcap output file
 * @param handle Output handle for polling, waiting, and cleanup
 * @return Status indicating whether submission succeeded
 */
Status daqiri_write_pcap_to_file_async(BurstParams *burst,
                                       const std::string &absolute_path,
                                       const std::string &file_prefix,
                                       FileWriteHandle **handle);

/**
 * @brief Poll an asynchronous burst file write.
 *
 * @param handle Handle returned by a daqiri_write_*_to_file_async() call
 * @param status Optional output status summary
 * @return SUCCESS when all packet writes are complete, NOT_READY while pending,
 * or an error status
 */
Status daqiri_file_write_poll(FileWriteHandle *handle, FileWriteStatus *status);

/**
 * @brief Wait for an asynchronous burst file write to complete.
 *
 * @param handle Handle returned by a daqiri_write_*_to_file_async() call
 * @param status Optional output status summary
 * @return SUCCESS when all packet writes are complete or an error status
 */
Status daqiri_file_write_wait(FileWriteHandle *handle, FileWriteStatus *status);

/**
 * @brief Destroy an asynchronous file write handle and release file write
 * resources.
 *
 * If I/O is still pending, this call waits for completion before releasing file
 * handles.
 *
 * @param handle Handle returned by a daqiri_write_*_to_file_async() call
 * @return SUCCESS when resources are released or an error status
 */
Status daqiri_file_write_destroy(FileWriteHandle *handle);

/**
 * @brief Create an S3 raw object writer.
 *
 * Credentials are resolved by the AWS SDK provider chain. endpoint_override and
 * path_style are only needed for S3-compatible services that require them.
 * Returns NOT_SUPPORTED when DAQIRI was built without DAQIRI_ENABLE_S3=ON.
 *
 * @param config S3 writer configuration
 * @param writer Output writer handle
 * @return Status indicating success or failure
 */
Status daqiri_s3_writer_create(const S3WriterConfig &config,
                               S3Writer **writer);

/**
 * @brief Asynchronously write each packet in a burst to a separate S3 object.
 *
 * Object keys are object_prefix_<packet_index>. The write path stages each
 * packet's logical bytes into DAQIRI-owned host memory before returning, so the
 * caller may free the burst after successful submission. No multipart upload is
 * performed; objects larger than the S3 single-PUT limit are not supported.
 *
 * @param writer Writer returned by daqiri_s3_writer_create()
 * @param burst Burst structure containing packets
 * @param object_prefix Prefix used for each S3 object key
 * @param packet_data_offset Bytes to skip from the start of each logical packet
 * @param handle Output handle for polling, waiting, and cleanup
 * @return Status indicating whether submission succeeded
 */
Status daqiri_write_raw_to_s3_objects_async(S3Writer *writer,
                                            BurstParams *burst,
                                            const std::string &object_prefix,
                                            uint64_t packet_data_offset,
                                            S3WriteHandle **handle);

/**
 * @brief Poll an asynchronous S3 write.
 *
 * @param handle Handle returned by daqiri_write_raw_to_s3_objects_async()
 * @param status Optional output status summary
 * @return SUCCESS when all uploads are complete, NOT_READY while pending, or an
 * error status
 */
Status daqiri_s3_write_poll(S3WriteHandle *handle, S3WriteStatus *status);

/**
 * @brief Wait for asynchronous S3 writes to complete.
 *
 * @param handle Handle returned by daqiri_write_raw_to_s3_objects_async()
 * @param status Optional output status summary
 * @return SUCCESS when all uploads are complete or an error status
 */
Status daqiri_s3_write_wait(S3WriteHandle *handle, S3WriteStatus *status);

/**
 * @brief Destroy an asynchronous S3 write handle.
 *
 * If uploads are still pending, this call waits for completion before
 * releasing staging buffers and request resources.
 *
 * @param handle Handle returned by daqiri_write_raw_to_s3_objects_async()
 * @return SUCCESS when resources are released or an error status
 */
Status daqiri_s3_write_destroy(S3WriteHandle *handle);

/**
 * @brief Destroy an S3 raw object writer.
 *
 * The caller must not use the writer after this call. S3WriteHandle instances
 * keep their own client references, so destroying a writer does not invalidate
 * already submitted asynchronous writes.
 *
 * @param writer Writer returned by daqiri_s3_writer_create()
 * @return SUCCESS when resources are released
 */
Status daqiri_s3_writer_destroy(S3Writer *writer);

/**
 * @brief Frees all segments of a single packet
 *
 * @param burst Burst structure containing packet lists
 * @param idx Index of packet
 */
void free_packet(BurstParams *burst, int idx);

/**
 * @brief Frees a single segment from a single packet
 *
 * @param burst Burst structure containing packet lists
 * @param seg Segment of packet in scatter list
 * @param idx Index of packet
 */
void free_packet_segment(BurstParams *burst, int seg, int idx);

/**
 * @brief Free all packets for a single segment in a burst
 *
 * Frees all packets in a single segment in a burst of packets.
 *
 * @param burst Burst structure containing packet lists
 */
void free_all_segment_packets(BurstParams *burst, int seg);

/**
 * @brief Free a receive burst
 *
 * Frees the buffer containing a receive burst buffer. This function does not
 * free packets; packets must be freed prior to calling this.
 *
 * @param burst
 */
void free_rx_burst(BurstParams *burst);

/**
 * @brief Free a transmit burst buffer
 *
 * Frees the buffer containing a transmit burst buffer. This function does not
 * free packets; packets must be freed prior to calling this.
 *
 * @param burst Burst structure to free
 */
void free_tx_burst(BurstParams *burst);

/**
 * @brief Free a receive TX meta buffer
 *
 * Frees the buffer containing a receive TX meta buffer. This function does not
 * free packets; packets must be freed prior to calling this.
 *
 * @param burst Burst structure to free
 */
void free_tx_metadata(BurstParams *burst);

/**
 * @brief Free a receive RX meta buffer
 *
 * Frees the buffer containing a receive RX meta buffer. This function does not
 * free packets; packets must be freed prior to calling this.
 *
 * @param burst Burst structure to free
 */
void free_rx_metadata(BurstParams *burst);

/**
 * @brief Get the number of packets in a burst
 *
 * @param burst Burst structure with packets
 */
int64_t get_num_packets(BurstParams *burst);

/**
 * @brief Get the queue ID of a burst
 *
 * @param burst Burst structure with packets
 */
int64_t get_q_id(BurstParams *burst);

/**
 * @brief Get the transport connection ID associated with a burst
 *
 * @param burst Burst structure with transport metadata
 */
uintptr_t get_connection_id(const BurstParams *burst);

/**
 * @brief Set the transport connection ID associated with a burst
 *
 * @param burst Burst structure with transport metadata
 * @param conn_id Connection ID representing a unique client/server connection
 */
void set_connection_id(BurstParams *burst, uintptr_t conn_id);

/**
 * @brief Get mac address of an interface
 *
 * @param port Port number of interface
 * @param mac MAC address of interface
 *
 * @returns Status::SUCCESS on success
 */
Status get_mac_addr(int port, char *mac);

/**
 * @brief Drop all traffic on a port
 *
 * Creates a high-priority flow rule that drops all incoming traffic on the
 * specified port. This acts as a "kill switch" for traffic. Use
 * allow_all_traffic() to remove the drop rule.
 *
 * @param port Port number of interface
 *
 * @returns Status::SUCCESS on success, error status on failure
 */
Status drop_all_traffic(int port);

/**
 * @brief Allow all traffic on a port
 *
 * Removes a previously installed drop rule created by drop_all_traffic(),
 * restoring normal traffic flow on the port.
 *
 * @param port Port number of interface
 *
 * @returns Status::SUCCESS on success, error status on failure
 */
Status allow_all_traffic(int port);

/**
 * @brief Get port number from interface name
 *
 * @param key PCIe address or config name of the interface to look up
 *
 * @returns Port number or -1 for not found
 */
int get_port_id(const std::string &key);

/**
 * @brief Set the number of packets in a burst
 *
 * @param burst Burst structure
 * @param num Number of packets
 */
void set_num_packets(BurstParams *burst, int64_t num);

/**
 * @brief Send a TX burst.
 *
 * Takes ownership of @p burst on SUCCESS and on NO_SPACE_AVAILABLE: on SUCCESS
 * the TX worker owns it; on NO_SPACE_AVAILABLE (TX ring full) it has already
 * freed the packets and the burst internally. In both cases the caller must
 * NOT free or otherwise access @p burst afterwards. NO_SPACE_AVAILABLE is the
 * only failure a correctly-configured sender hits at runtime.
 *
 * @param burst Burst structure
 * @return Status indicating result. Valid values are:
 *    SUCCESS: burst enqueued to the TX worker (ownership transferred)
 *    NO_SPACE_AVAILABLE: TX ring full; burst already freed (ownership consumed)
 *    INVALID_PARAMETER: bad port/queue; burst NOT consumed (see issue #164)
 */
Status send_tx_burst(BurstParams *burst);

/**
 * @brief Get a RX burst
 *
 * @param burst Burst structure
 * @param port Port ID of interface
 * @param q Queue ID of interface
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Burst received successfully
 *    NULL_PTR: No bursts ready to receive
 */
Status get_rx_burst(BurstParams **burst, int port, int q);

/**
 * @brief Get a RX burst from any queue on a specific port
 *
 * @param burst Burst structure
 * @param port Port ID of interface
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Burst received successfully
 *    NULL_PTR: No bursts ready to receive on any queue for this port
 */
Status get_rx_burst(BurstParams **burst, int port);

/**
 * @brief Get a RX burst from any queue on any port
 *
 * @param burst Burst structure
 * @return Status indicating status. Valid values are:
 *    SUCCESS: Burst received successfully
 *    NULL_PTR: No bursts ready to receive on any queue on any port
 */
Status get_rx_burst(BurstParams **burst);

/**
 * @brief Get a RX burst
 *
 * @param burst Burst structure
 * @param conn_id Connection ID representing a unique ID for a client/server
 * connection
 * @param server True if server, false if client. Used to determine which ring
 * to dequeue from.
 */
Status get_rx_burst(BurstParams **burst, uintptr_t conn_id, bool server);

/**
 * @brief Set CUDA stream for a configured GPU reorder plan
 *
 * @param interface_name Interface name from config
 * @param reorder_name Reorder config name from
 * interfaces[].rx.reorder_configs[]
 * @param stream CUDA stream used for this plan. CPU reorder plans do not
 * require a stream.
 * @return Status indicating success or failure
 */
Status set_reorder_cuda_stream(const std::string &interface_name,
                               const std::string &reorder_name,
                               cudaStream_t stream);

/**
 * @brief Get metadata for a reordered RX burst
 *
 * @param burst Reordered burst returned by get_rx_burst()
 * @param info Output reorder metadata. For GPU reorder, this is valid after
 * burst->event completes.
 * @return Status indicating success or failure
 */
Status get_reorder_burst_info(BurstParams *burst, ReorderBurstInfo *info);

/**
 * @brief Set the header fields in a burst
 *
 * @param burst Burst structure
 * @param port Port ID of interface
 * @param q Queue ID of interface
 * @param num Number of packets
 * @param segs Number of segments
 */
void set_header(BurstParams *burst, uint16_t port, uint16_t q, int64_t num,
                int segs);

/**
 * @brief Format MAC address string to char buffer
 *
 * @param dst Destination buffer of at least six bytes. Invalid input zeroes
 * the six-byte destination.
 * @param addr MAC address as string in format xx:xx:xx:xx:xx:xx, with exactly
 * two hexadecimal digits per octet.
 */
void format_eth_addr(char *dst, std::string addr);

/**
 * @brief Shut down DAQIRI and release the active engine and its resources.
 * A subsequent daqiri_init() creates a fresh engine instance.
 *
 */
void shutdown();

/**
 * @brief Print port statistics
 *
 */
void print_stats();

/**
 * @brief Get the number of RX queues. May be overridden by the engine if the
 * number of queues differs from what is defined in the config.
 *
 * @param port_id Port ID of interface
 * @return uint16_t Number of RX queues
 */
uint16_t get_num_rx_queues(int port_id);

/**
 * @brief Flush all packets from a specific port/queue
 *
 * Drains and discards all packets currently in the specified queue on the
 * specified port. This is useful for clearing stale packets from a queue before
 * starting operations.
 *
 * @param port Port number of interface
 * @param queue Queue ID on the port
 */
void flush_port_queue(int port, int queue);

// Generic socket functions
Status socket_connect_to_server(const std::string &server_addr,
                                uint16_t server_port, uintptr_t *conn_id);
Status socket_connect_to_server(const std::string &server_addr,
                                uint16_t server_port,
                                const std::string &src_addr,
                                uintptr_t *conn_id);
Status socket_get_port_queue(uintptr_t conn_id, uint16_t *port,
                             uint16_t *queue);
Status socket_get_server_conn_id(const std::string &server_addr,
                                 uint16_t server_port, uintptr_t *conn_id);
Status socket_setsockopt(uintptr_t conn_id, int level, int optname,
                         const void *optval, size_t optlen);

// RDMA functions
Status rdma_connect_to_server(const std::string &server_addr,
                              uint16_t server_port, uintptr_t *conn_id);
Status rdma_connect_to_server(const std::string &server_addr,
                              uint16_t server_port, const std::string &src_addr,
                              uintptr_t *conn_id);
Status rdma_get_port_queue(uintptr_t conn_id, uint16_t *port, uint16_t *queue);
Status rdma_get_server_conn_id(const std::string &server_addr,
                               uint16_t server_port, uintptr_t *conn_id);
Status rdma_set_header(BurstParams *burst, RDMAOpCode op_code,
                       uintptr_t conn_id, bool is_server, int num_pkts,
                       uint64_t wr_id, const std::string &local_mr_name);
RDMAOpCode rdma_get_opcode(BurstParams *burst);

}; // namespace daqiri

template <> struct YAML::convert<daqiri::NetworkConfig> {
  static Node encode(const daqiri::NetworkConfig &input_spec) {
    Node node;
    // node["type"] = inputTypeToString(input_spec.type_);
    // node["name"] = input_spec.tensor_name_;
    // node["opacity"] = std::to_string(input_spec.opacity_);
    // node["priority"] = std::to_string(input_spec.priority_);
    // node["color"] = input_spec.color_;
    // node["line_width"] = std::to_string(input_spec.line_width_);
    // node["point_size"] = std::to_string(input_spec.point_size_);
    // node["text"] = input_spec.text_;
    // node["depth_map_render_mode"] =
    //      depthMapRenderModeToString(input_spec.depth_map_render_mode_);
    return node;
  }
  /**
   * @brief Parse flow configuration from a YAML node.
   *
   * @param flow_item The YAML node containing the flow configuration.
   * @param flow The FlowConfig object to populate.
   * @return true if parsing was successful, false otherwise.
   */
  static bool parse_flow_config(const YAML::Node &flow_item,
                                daqiri::FlowConfig &flow);

  /**
   * @brief Parse flex item configuration from a YAML node.
   *
   * @param flex_item The YAML node containing the flex item configuration.
   * @param flex_item_config The FlexItemConfig object to populate.
   * @return true if parsing was successful, false otherwise.
   */
  static bool parse_flex_item_config(const YAML::Node &flex_item,
                                     daqiri::FlexItemConfig &flex_item_config);

  /**
   * @brief Parse reorder configuration from a YAML node.
   *
   * @param reorder_item The YAML node containing the reorder configuration.
   * @param reorder_config The ReorderConfig object to populate.
   * @return true if parsing was successful, false otherwise.
   */
  static bool parse_reorder_config(const YAML::Node &reorder_item,
                                   daqiri::ReorderConfig &reorder_config);

  /**
   * @brief Parse memory region configuration from a YAML node.
   *
   * @param mr The YAML node containing the memory region configuration.
   * @param tmr The MemoryRegionConfig object to populate.
   * @return true if parsing was successful, false otherwise.
   */
  static bool
  parse_memory_region_config(const YAML::Node &mr,
                             daqiri::MemoryRegionConfig &memory_region);

  /**
   * @brief Parse socket endpoint configuration from a YAML node.
   *
   * @param socket_item The YAML node containing socket configuration.
   * @param socket_cfg The SocketConfig object to populate.
   * @param protocol The inferred socket/RDMA transport protocol.
   * @return true if parsing was successful, false otherwise.
   */
  static bool parse_socket_config(const YAML::Node &socket_item,
                                  daqiri::SocketConfig &socket_cfg,
                                  daqiri::SocketProtocol &protocol);

  /**
   * @brief Parse RoCE transport configuration from a YAML node.
   *
   * @param roce_item The YAML node containing RoCE configuration.
   * @param roce_cfg The RoCEConfig object to populate.
   * @return true if parsing was successful, false otherwise.
   */
  static bool parse_roce_config(const YAML::Node &roce_item,
                                daqiri::RoCEConfig &roce_cfg);

  /**
   * @brief Parse common RX queue configuration from a YAML node.
   *
   * @param q_item The YAML node containing the RX queue configuration.
   * @param q The RxQueueConfig object to populate.
   * @param parse_memory_regions True if memory regions should be parsed, false
   * otherwise.
   * @return true if parsing was successful, false otherwise.
   */
  static bool parse_rx_queue_config(const YAML::Node &q_item,
                                    const daqiri::EngineType &engine_type,
                                    daqiri::RxQueueConfig &rx_queue_config,
                                    bool parse_memory_regions = true);

  /**
   * @brief Parse RX queue configuration from a YAML node.
   *
   * @param q_item The YAML node containing the RX queue configuration.
   * @param engine_type The engine type.
   * @param q The RxQueueConfig object to populate.
   * @param parse_memory_regions True if memory regions should be parsed, false
   * otherwise.
   * @return true if parsing was successful, false otherwise.
   */
  static bool
  parse_rx_queue_common_config(const YAML::Node &q_item,
                               daqiri::RxQueueConfig &rx_queue_config,
                               bool parse_memory_regions);

  /**
   * @brief Parse common TX queue configuration from a YAML node.
   *
   * @param q_item The YAML node containing the TX queue configuration.
   * @param q The TxQueueConfig object to populate.
   * @param parse_memory_regions True if memory regions should be parsed, false
   * otherwise.
   * @return true if parsing was successful, false otherwise.
   */
  static bool parse_tx_queue_config(const YAML::Node &q_item,
                                    const daqiri::EngineType &engine_type,
                                    daqiri::TxQueueConfig &tx_queue_config,
                                    bool parse_memory_regions);

  /**
   * @brief Parse TX queue configuration from a YAML node.
   *
   * @param q_item The YAML node containing the TX queue configuration.
   * @param engine_type The engine type.
   * @param q The TxQueueConfig object to populate.
   * @param parse_memory_regions True if memory regions should be parsed, false
   * otherwise.
   * @return true if parsing was successful, false otherwise.
   */
  static bool
  parse_tx_queue_common_config(const YAML::Node &q_item,
                               daqiri::TxQueueConfig &tx_queue_config,
                               bool parse_memory_regions);

  /**
   * @brief Decode the YAML node into an NetworkConfig object.
   *
   * This function parses the provided YAML node and populates the given
   * NetworkConfig object. It handles various configurations such as version,
   * master core, engine type, debug flag, memory regions, interfaces, RX
   * queues, TX queues, and flows.
   *
   * @param node The YAML node containing the configuration.
   * @param input_spec The NetworkConfig object to populate.
   * @return true if decoding was successful, false otherwise.
   */
  static bool decode(const Node &node, daqiri::NetworkConfig &input_spec) {
    if (!node.IsMap()) {
      DAQIRI_LOG_ERROR("InputSpec: expected a map");
      return false;
    }

    // YAML is using exceptions, catch them
    try {
      if (node["manager"].IsDefined()) {
        DAQIRI_LOG_ERROR(
            "Legacy 'manager' is no longer supported. Use 'stream_type' and "
            "'engine' instead.");
        return false;
      }

      input_spec.common_.version = node["version"].as<int32_t>();
      input_spec.common_.master_core_ = node["master_core"].as<int32_t>();

      if (!node["stream_type"].IsDefined()) {
        DAQIRI_LOG_ERROR(
            "Missing required field 'stream_type' (valid: 'raw' or 'socket')");
        return false;
      }

      input_spec.common_.stream_type = daqiri::stream_type_from_string(
          node["stream_type"].as<std::string>());
      if (input_spec.common_.stream_type == daqiri::StreamType::INVALID) {
        DAQIRI_LOG_ERROR("Invalid stream_type '{}'. Valid values: raw, socket",
                         node["stream_type"].as<std::string>());
        return false;
      }

      const bool socket_used =
          input_spec.common_.stream_type == daqiri::StreamType::SOCKET;
      if (node["engine"].IsDefined()) {
        input_spec.common_.engine = daqiri::config_engine_from_string(
            node["engine"].as<std::string>(), input_spec.common_.stream_type);
        if (input_spec.common_.engine != daqiri::EngineType::DEFAULT &&
            !daqiri::engine_type_supports_stream_type(
                input_spec.common_.engine, input_spec.common_.stream_type)) {
          DAQIRI_LOG_ERROR("engine '{}' is not valid for stream_type '{}'",
                           node["engine"].as<std::string>(),
                           daqiri::stream_type_to_string(
                               input_spec.common_.stream_type));
          return false;
        }
      } else {
        input_spec.common_.engine = daqiri::EngineType::DEFAULT;
      }

      // 'protocol' is no longer a config field. The transport is encoded in the
      // endpoint URI scheme (udp://, tcp://, roce://) and derived per-interface
      // in parse_socket_config. Reject any leftover 'protocol' key.
      if (node["protocol"].IsDefined()) {
        DAQIRI_LOG_ERROR(
            "Legacy 'protocol' is no longer supported. Encode the transport in "
            "the endpoint URI scheme (udp://, tcp://, roce://) instead.");
        return false;
      }

      // An explicit engine: ibverbs implies RoCE; otherwise the protocol is
      // derived from the endpoint URI scheme during interface parsing.
      input_spec.common_.protocol =
          (socket_used && input_spec.common_.engine == daqiri::EngineType::RDMA)
              ? daqiri::SocketProtocol::ROCE
              : daqiri::SocketProtocol::INVALID;

      if (socket_used &&
          daqiri::is_explicit_engine_type(input_spec.common_.engine) &&
          input_spec.common_.protocol != daqiri::SocketProtocol::INVALID &&
          !daqiri::engine_type_supports_socket_protocol(
              input_spec.common_.engine, input_spec.common_.protocol)) {
        DAQIRI_LOG_ERROR("engine '{}' is not valid for transport '{}'",
                         daqiri::config_engine_to_string(input_spec.common_.engine),
                         daqiri::socket_protocol_to_string(input_spec.common_.protocol));
        return false;
      }

      auto resolve_engine_type = [&input_spec, socket_used]() -> bool {
        if (daqiri::is_explicit_engine_type(input_spec.common_.engine)) {
          input_spec.common_.engine_type = input_spec.common_.engine;
          return true;
        }

        if (socket_used &&
            input_spec.common_.protocol == daqiri::SocketProtocol::INVALID) {
          input_spec.common_.engine_type = daqiri::EngineType::UNKNOWN;
          return true;
        }

        input_spec.common_.engine_type = daqiri::engine_type_from_stream_type(
            input_spec.common_.stream_type, input_spec.common_.protocol);
        input_spec.common_.engine = input_spec.common_.engine_type;
        return input_spec.common_.engine_type != daqiri::EngineType::UNKNOWN;
      };

      if (!resolve_engine_type()) {
        DAQIRI_LOG_ERROR("Unable to infer engine for stream_type '{}'",
                         daqiri::stream_type_to_string(
                             input_spec.common_.stream_type));
        return false;
      }

      input_spec.common_.loopback_ = daqiri::LoopbackType::DISABLED;
      try {
        const auto lbstr = node["loopback"].as<std::string>();
        if (lbstr == "sw") {
          input_spec.common_.loopback_ = daqiri::LoopbackType::LOOPBACK_TYPE_SW;
        } else if (!lbstr.empty()) {
          DAQIRI_LOG_ERROR(
              "Invalid loopback type: {}. Use 'sw' or empty string ''", lbstr);
          return false;
        }
      } catch (const std::exception &e) {
      }

      try {
        input_spec.debug_ = node["debug"].as<bool>(false);
      } catch (const std::exception &e) {
        input_spec.debug_ = false;
      }

      try {
        input_spec.log_level_ =
            daqiri::LogLevel::from_string(node["log_level"].as<std::string>(
                daqiri::LogLevel::to_string(daqiri::LogLevel::WARN)));
      } catch (const std::exception &e) {
        input_spec.log_level_ = daqiri::LogLevel::WARN;
      }

      try {
        input_spec.tx_meta_buffers_ = node["tx_meta_buffers"].as<uint32_t>(
            daqiri::DEFAULT_TX_META_BUFFERS);
      } catch (const std::exception &e) {
        input_spec.tx_meta_buffers_ = daqiri::DEFAULT_TX_META_BUFFERS;
      }

      try {
        input_spec.rx_meta_buffers_ = node["rx_meta_buffers"].as<uint32_t>(
            daqiri::DEFAULT_RX_META_BUFFERS);
      } catch (const std::exception &e) {
        input_spec.rx_meta_buffers_ = daqiri::DEFAULT_RX_META_BUFFERS;
      }

      try {
        const auto &mrs = node["memory_regions"];
        for (const auto &mr : mrs) {
          daqiri::MemoryRegionConfig tmr;
          if (!parse_memory_region_config(mr, tmr)) {
            DAQIRI_LOG_ERROR("Failed to parse memory region config");
            return false;
          }
          if (input_spec.mrs_.find(tmr.name_) != input_spec.mrs_.end()) {
            DAQIRI_LOG_CRITICAL("Duplicate memory region names: {}", tmr.name_);
            return false;
          }
          input_spec.mrs_[tmr.name_] = tmr;
        }
      } catch (const std::exception &e) {
        DAQIRI_LOG_ERROR("Must define at least one memory type");
        return false;
      }

      try {
        const auto &intfs = node["interfaces"];
        for (const auto &intf : intfs) {
          daqiri::InterfaceConfig ifcfg;

          ifcfg.name_ = intf["name"].as<std::string>();
          ifcfg.address_ = intf["address"].as<std::string>();

          if (intf["rdma_config"].IsDefined()) {
            DAQIRI_LOG_ERROR("Legacy 'rdma_config' is no longer supported. Use "
                             "'socket_config' and 'roce_config' instead.");
            return false;
          }

          bool roce_used = false;
          if (socket_used) {
            if (!intf["socket_config"].IsDefined()) {
              DAQIRI_LOG_ERROR("Missing 'socket_config' for interface '{}'",
                               ifcfg.name_);
              return false;
            }
            if (!parse_socket_config(intf["socket_config"],
                                     ifcfg.socket_,
                                     input_spec.common_.protocol)) {
              DAQIRI_LOG_ERROR(
                  "Failed to parse 'socket_config' for interface '{}'",
                  ifcfg.name_);
              return false;
            }
            if (!resolve_engine_type()) {
              DAQIRI_LOG_ERROR("Unable to infer engine for interface '{}'",
                               ifcfg.name_);
              return false;
            }
            if (daqiri::is_explicit_engine_type(input_spec.common_.engine) &&
                !daqiri::engine_type_supports_socket_protocol(
                    input_spec.common_.engine, input_spec.common_.protocol)) {
              DAQIRI_LOG_ERROR("engine '{}' is not valid for address scheme "
                               "on interface '{}'",
                               daqiri::config_engine_to_string(
                                   input_spec.common_.engine),
                               ifcfg.name_);
              return false;
            }

            roce_used =
                input_spec.common_.protocol == daqiri::SocketProtocol::ROCE;
            if (roce_used) {
              if (!intf["roce_config"].IsDefined()) {
                DAQIRI_LOG_ERROR("Missing 'roce_config' for interface '{}' "
                                 "with roce:// endpoint",
                                 ifcfg.name_);
                return false;
              }

              if (!parse_roce_config(intf["roce_config"], ifcfg.roce_)) {
                DAQIRI_LOG_ERROR(
                    "Failed to parse 'roce_config' for interface '{}'",
                    ifcfg.name_);
                return false;
              }

              ifcfg.rdma_.mode_ =
                  ifcfg.socket_.mode_ == daqiri::SocketMode::SERVER
                      ? daqiri::RDMAMode::SERVER
                      : daqiri::RDMAMode::CLIENT;
              ifcfg.rdma_.xmode_ = ifcfg.roce_.transport_mode_;
              ifcfg.rdma_.port_ = ifcfg.socket_.local_port_;
              if (ifcfg.socket_.mode_ == daqiri::SocketMode::CLIENT &&
                  ifcfg.rdma_.port_ == 0) {
                ifcfg.rdma_.port_ = ifcfg.socket_.remote_port_;
              }
            } else if (intf["roce_config"].IsDefined()) {
              DAQIRI_LOG_ERROR(
                  "'roce_config' is only valid with a roce:// endpoint "
                  "(interface '{}')",
                  ifcfg.name_);
              return false;
            }
          } else {
            if (intf["socket_config"].IsDefined() ||
                intf["roce_config"].IsDefined()) {
              DAQIRI_LOG_ERROR("'socket_config'/'roce_config' are only valid "
                               "for stream_type 'socket' "
                               "(interface '{}')",
                               ifcfg.name_);
              return false;
            }
          }

          try {
            const auto &rx = intf["rx"];
            daqiri::RxConfig rx_cfg;

            try {
              rx_cfg.flow_isolation_ = rx["flow_isolation"].as<bool>();
            } catch (const std::exception& e) { rx_cfg.flow_isolation_ = false; }

            try {
              rx_cfg.dynamic_flow_capacity_ =
                  rx["dynamic_flow_capacity"].as<uint32_t>();
            } catch (const std::exception& e) {
              rx_cfg.dynamic_flow_capacity_ = daqiri::DEFAULT_DYNAMIC_FLOW_CAPACITY;
            }

            for (const auto &q_item : rx["queues"]) {
              daqiri::RxQueueConfig q;
              if (!parse_rx_queue_config(
                      q_item, input_spec.common_.engine_type, q, !roce_used)) {
                DAQIRI_LOG_ERROR("Failed to parse RxQueueConfig");
                return false;
              }

              try {
                q.timeout_us_ = q_item["timeout_us"].as<uint64_t>();
              } catch (const std::exception &e) {
                q.timeout_us_ = 0;
              }

              rx_cfg.queues_.emplace_back(std::move(q));
            }

            if (rx["flows"].IsDefined()) {
              if (!rx["flows"].IsSequence()) {
                DAQIRI_LOG_ERROR("'rx.flows' must be a sequence for interface '{}'",
                                 ifcfg.name_);
                return false;
              }
              for (const auto &flow_item : rx["flows"]) {
                daqiri::FlowConfig flow;
                if (!parse_flow_config(flow_item, flow)) {
                  DAQIRI_LOG_ERROR("Failed to parse FlowConfig");
                  return false;
                }
                rx_cfg.flows_.emplace_back(std::move(flow));
              }
            }

            try {
              for (const auto &flex_item : rx["flex_items"]) {
                daqiri::FlexItemConfig flex_item_config;
                if (!parse_flex_item_config(flex_item, flex_item_config)) {
                  DAQIRI_LOG_ERROR("Failed to parse FlexItemConfig");
                  return false;
                }
                rx_cfg.flex_items_.emplace_back(std::move(flex_item_config));
              }
            } catch (const std::exception &e) {
            } // No flex_items defined for this interface.

            try {
              std::unordered_set<std::string> reorder_names;
              for (const auto &reorder_item : rx["reorder_configs"]) {
                daqiri::ReorderConfig reorder_cfg;
                if (!parse_reorder_config(reorder_item, reorder_cfg)) {
                  DAQIRI_LOG_ERROR("Failed to parse ReorderConfig");
                  return false;
                }
                if (reorder_names.find(reorder_cfg.name_) !=
                    reorder_names.end()) {
                  DAQIRI_LOG_ERROR(
                      "Duplicate reorder config name '{}' in interface '{}'",
                      reorder_cfg.name_, ifcfg.name_);
                  return false;
                }
                reorder_names.insert(reorder_cfg.name_);
                rx_cfg.reorder_configs_.emplace_back(std::move(reorder_cfg));
              }
            } catch (const std::exception &e) {
            } // No reorder_configs defined for this interface.

            ifcfg.rx_ = rx_cfg;
          } catch (const std::exception &e) {
          } // No RX queues defined for this interface.

          try {
            const auto &tx = intf["tx"];
            daqiri::TxConfig tx_cfg;

            try {
              tx_cfg.accurate_send_ = tx["accurate_send"].as<bool>();
            } catch (const std::exception &e) {
              tx_cfg.accurate_send_ = false;
            }

            for (const auto &q_item : tx["queues"]) {
              daqiri::TxQueueConfig q;
              if (!parse_tx_queue_config(
                      q_item, input_spec.common_.engine_type, q, !roce_used)) {
                DAQIRI_LOG_ERROR("Failed to parse TxQueueConfig");
                return false;
              }
              tx_cfg.queues_.emplace_back(std::move(q));
            }

            for (const auto &flow_item : tx["flows"]) {
              daqiri::FlowConfig flow;
              if (!parse_flow_config(flow_item, flow)) {
                DAQIRI_LOG_ERROR("Failed to parse TX FlowConfig");
                return false;
              }
              tx_cfg.flows_.emplace_back(std::move(flow));
            }

            ifcfg.tx_ = tx_cfg;
          } catch (const std::exception &e) {
          } // No TX queues defined for this interface.

          input_spec.ifs_.push_back(ifcfg);
        }
      } catch (const std::exception &e) {
        DAQIRI_LOG_ERROR(e.what());
        return false;
      }

      if (socket_used &&
          input_spec.common_.protocol == daqiri::SocketProtocol::INVALID) {
        DAQIRI_LOG_ERROR(
            "Missing socket protocol. Use local_addr/remote_addr URI schemes.");
        return false;
      }

      if (socket_used &&
          (input_spec.common_.protocol == daqiri::SocketProtocol::TCP ||
           input_spec.common_.protocol == daqiri::SocketProtocol::UDP)) {
        std::unordered_set<std::string> gpu_mrs;
        for (const auto &intf : input_spec.ifs_) {
          for (const auto &q : intf.rx_.queues_) {
            for (const auto &mr_name : q.common_.mrs_) {
              const auto it = input_spec.mrs_.find(mr_name);
              if (it != input_spec.mrs_.end() &&
                  it->second.kind_ == daqiri::MemoryKind::DEVICE) {
                gpu_mrs.insert(mr_name);
              }
            }
          }
          for (const auto &q : intf.tx_.queues_) {
            for (const auto &mr_name : q.common_.mrs_) {
              const auto it = input_spec.mrs_.find(mr_name);
              if (it != input_spec.mrs_.end() &&
                  it->second.kind_ == daqiri::MemoryKind::DEVICE) {
                gpu_mrs.insert(mr_name);
              }
            }
          }
        }

        if (!gpu_mrs.empty()) {
          std::string joined;
          for (const auto &mr_name : gpu_mrs) {
            if (!joined.empty()) {
              joined += ", ";
            }
            joined += mr_name;
          }
          DAQIRI_LOG_ERROR(
              "GPU memory regions are not supported for protocol '{}'. "
              "Offending "
              "memory_regions: {}",
              daqiri::socket_protocol_to_string(input_spec.common_.protocol),
              joined);
          return false;
        }
      }

      DAQIRI_LOG_INFO("Finished reading DAQIRI configuration");

      return true;
    } catch (const std::exception &e) {
      DAQIRI_LOG_ERROR(e.what());
      return false;
    }
  }
};

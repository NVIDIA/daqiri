/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MGBE_PMD_INTERNAL_H
#define MGBE_PMD_INTERNAL_H
struct ether_priv_data;
struct net_device;
bool mgbe_pmd_owned(struct ether_priv_data* pdata);
int mgbe_pmd_attach_rings(struct ether_priv_data* pdata);
int mgbe_pmd_net_open(struct net_device* ndev);
void mgbe_pmd_register(struct ether_priv_data* pdata);
bool mgbe_pmd_unregister(struct ether_priv_data* pdata);
void mgbe_pmd_platform_shutdown(struct ether_priv_data* pdata);
bool mgbe_pmd_link_event(struct ether_priv_data* pdata, unsigned int enable);
#endif

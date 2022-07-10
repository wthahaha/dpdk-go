#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_bus_pci.h>
#include <rte_kni.h>

//#define DEBUG
//#define SINGLE_CORE
//#define IDEL_SLEEP
//#define KNI_BYPASS_TCP_UDP
char *VERSION = "1.0.0";

#define RING_BUFFER_SIZE 134217728

#define KNI_ENET_HEADER_SIZE 14
#define KNI_ENET_FCS_SIZE 4
#define KNI_RX_RING_SIZE 2048
#define KNI_MAX_PACKET_SIZE 2048

#define NUM_MBUFS 524288
#define MBUF_CACHE_SIZE 512
#define RX_QUEUE 1
#define TX_QUEUE 1
#define RX_RING_SIZE 2048
#define TX_RING_SIZE 2048
#define BURST_SIZE 64
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1
#define RTE_DEV_NAME_MAX_LEN 512

uint8_t *mem_send_head = NULL;
uint8_t *mem_send_cur = NULL;
uint8_t *mem_recv_head = NULL;
uint8_t *mem_recv_cur = NULL;
volatile uint8_t *send_pos_pointer = NULL;
volatile uint8_t *recv_pos_pointer = NULL;

struct rte_kni *kni;

static volatile bool force_quit = false;
uint16_t used_port_id = 0;
static struct rte_eth_conf port_conf_default;
struct rte_mempool *mbuf_pool = NULL;

// ==========环状缓冲区 BEGIN==========
/*
0				8				16			24				32				...
+---------------------------------------------------------------------------+
|	len low		|	len high	|	finish	|	mem align	|	raw data	|
+---------------------------------------------------------------------------+
*/

// 分配发送缓冲区内存空间
void *alloc_send_mem(void) {
    mem_send_head = (uint8_t *) malloc(RING_BUFFER_SIZE);
    memset(mem_send_head, 0x00, RING_BUFFER_SIZE);
    return (void *) mem_send_head;
}

// 分配接收缓冲区内存空间
void *alloc_recv_mem(void) {
    mem_recv_head = (uint8_t *) malloc(RING_BUFFER_SIZE);
    memset(mem_recv_head, 0x00, RING_BUFFER_SIZE);
    recv_pos_pointer = mem_recv_head;
    return (void *) mem_recv_head;
}

// 发送缓冲区当前已读指针位置
void **send_pos_pointer_addr(void) {
    return (void **) (&send_pos_pointer);
}

// 接收缓冲区当前已读指针位置
void **recv_pos_pointer_addr(void) {
    return (void **) (&recv_pos_pointer);
}

// 环状缓冲区写入
uint8_t write_recv_mem_core(uint8_t *data, uint16_t len) {
    // 内存对齐
    uint8_t aling_size = len % 4;
    if (aling_size != 0) {
        aling_size = 4 - aling_size;
    }
    len += aling_size;
    uint32_t head_u32 = 0;
    head_u32 = (((uint32_t) data[0]) << 0) +
               (((uint32_t) data[1]) << 8) +
               (((uint32_t) data[2]) << 16) +
               (((uint32_t) data[3]) << 24);
    int32_t overflow = (mem_recv_cur + len) - (mem_recv_head + RING_BUFFER_SIZE);
#ifdef DEBUG
    printf("[write_recv_mem_core] overflow: %d, len: %d, mem_recv_cur: %p, mem_recv_head: %p, recv_pos_pointer: %p\n",
           overflow, len, mem_recv_cur, mem_recv_head, recv_pos_pointer);
#endif
    if (overflow >= 0) {
        // 有溢出
        if (mem_recv_cur < recv_pos_pointer) {
            // 已经处于读写指针交叉状态 丢弃数据
            return 1;
        }
        if (overflow >= (recv_pos_pointer - mem_recv_head)) {
            // 即使进入读写指针交叉状态 剩余内存依然不足 丢弃数据
            return 1;
        }
        uint32_t *head_ptr = (uint32_t *) mem_recv_cur;
        // 写入头部 原子操作
        *head_ptr = head_u32;
        if ((len - overflow) > 4) {
            // 拷贝前半段数据
            memcpy(mem_recv_cur + 4, data + 4, (len - overflow) - 4);
        }
        mem_recv_cur = mem_recv_head;
        // 拷贝后半段数据
        memcpy(mem_recv_cur, data + 4 + ((len - overflow) - 4), overflow);
        mem_recv_cur += overflow;
        if (mem_recv_cur >= mem_recv_head + RING_BUFFER_SIZE) {
            mem_recv_cur = mem_recv_head;
        }
    } else {
        // 无溢出
        if ((mem_recv_cur < recv_pos_pointer) && ((mem_recv_cur + len) >= recv_pos_pointer)) {
            // 状态下剩余内存不足 丢弃数据
            return 1;
        }
        uint32_t *head_ptr = (uint32_t *) mem_recv_cur;
        // 写入头部 原子操作
        *head_ptr = head_u32;
        memcpy(mem_recv_cur + 4, data + 4, len - 4);
        mem_recv_cur += len;
        if (mem_recv_cur >= mem_recv_head + RING_BUFFER_SIZE) {
            mem_recv_cur = mem_recv_head;
        }
    }
    return 0;
}

// 写入接收缓冲区
void write_recv_mem(uint8_t *data, uint16_t len) {
    if (mem_recv_cur == NULL) {
        mem_recv_cur = mem_recv_head;
    }
    if (len > 1514) {
        return;
    }
    // 4字节头部 + 最大1514字节数据 + 2字节内存对齐
    uint8_t data_pkg[4 + 1514 + 2];
    memset(data_pkg, 0x00, 1520);
    // 数据长度标识
    data_pkg[0] = (uint8_t)(len);
    data_pkg[1] = (uint8_t)(len >> 8);
    // 写入完成标识
    uint8_t *finish_flag_pointer = mem_recv_cur + 2;
    data_pkg[2] = 0x00;
    // 内存对齐
    data_pkg[3] = 0x00;
    // 写入数据
    memcpy(data_pkg + 4, data, len);
    uint8_t ret = write_recv_mem_core(data_pkg, len + 4);
    if (ret == 1) {
        return;
    }
    // 将后4个字节即长度标识与写入完成标识的内存置为0x00
    mem_recv_cur[0] = 0x00;
    mem_recv_cur[1] = 0x00;
    mem_recv_cur[2] = 0x00;
    mem_recv_cur[3] = 0x00;
    // 修改写入完成标识 原子操作
    *finish_flag_pointer = 0x01;
}

// 读取发送缓冲区
void read_send_mem(uint8_t *data, uint16_t *len) {
    if (mem_send_cur == NULL) {
        mem_send_cur = mem_send_head;
    }
    *len = 0;
    // 读取头部 原子操作
    uint32_t *head_ptr = (uint32_t *) mem_send_cur;
    uint32_t head_u32 = *head_ptr;
    *len = (uint16_t) head_u32;
    if (*len == 0) {
        // 没有新数据
        return;
    }
    uint8_t finish_flag = (uint8_t)(head_u32 >> 16);
    if (finish_flag == 0x00) {
        // 数据尚未写入完成
        *len = 0;
        return;
    }
    mem_send_cur += 4;
    if (mem_send_cur >= mem_send_head + RING_BUFFER_SIZE) {
        mem_send_cur = mem_send_head;
    }
    int32_t overflow = (mem_send_cur + *len) - (mem_send_head + RING_BUFFER_SIZE);
#ifdef DEBUG
    printf("[read_send_mem] overflow: %d, len: %d, mem_send_cur: %p, mem_send_head: %p, send_pos_pointer: %p\n",
           overflow, *len, mem_send_cur, mem_send_head, send_pos_pointer);
#endif
    if (overflow >= 0) {
        // 拷贝前半段数据
        memcpy(data, mem_send_cur, *len - overflow);
        mem_send_cur = mem_send_head;
        // 拷贝后半段数据
        memcpy(data + (*len - overflow), mem_send_cur, *len - (*len - overflow));
        // 内存对齐
        uint8_t aling_size = overflow % 4;
        if (aling_size != 0) {
            aling_size = 4 - aling_size;
        }
        mem_send_cur += overflow + aling_size;
        if (mem_send_cur >= mem_send_head + RING_BUFFER_SIZE) {
            mem_send_cur = mem_send_head;
        }
        send_pos_pointer = mem_send_cur;
    } else {
        memcpy(data, mem_send_cur, *len);
        // 内存对齐
        uint8_t aling_size = *len % 4;
        if (aling_size != 0) {
            aling_size = 4 - aling_size;
        }
        mem_send_cur += *len + aling_size;
        if (mem_send_cur >= mem_send_head + RING_BUFFER_SIZE) {
            mem_send_cur = mem_send_head;
        }
        send_pos_pointer = mem_send_cur;
    }
}
// ==========环状缓冲区 END==========

// ==========KNI BEGIN==========
void update_kni_link_status(uint16_t port_id) {
    struct rte_eth_link link;
    memset(&link, 0, sizeof(link));
    rte_eth_link_get_nowait(port_id, &link);
    rte_kni_update_link(kni, link.link_status);
}

static int kni_change_mtu(uint16_t port_id, unsigned int new_mtu) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        RTE_LOG(ERR, APP, "invalid port id %u\n", port_id);
        return -EINVAL;
    }

    RTE_LOG(INFO, APP, "change mtu of port %u to %u\n", port_id, new_mtu);

    rte_eth_dev_stop(port_id);

    struct rte_eth_conf port_conf = port_conf_default;
    port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
    if (new_mtu > ETHER_MAX_LEN) {
        port_conf.rxmode.offloads |= DEV_RX_OFFLOAD_JUMBO_FRAME;
    } else {
        port_conf.rxmode.offloads &= ~DEV_RX_OFFLOAD_JUMBO_FRAME;
    }
    port_conf.rxmode.max_rx_pkt_len = new_mtu + KNI_ENET_HEADER_SIZE + KNI_ENET_FCS_SIZE;

    int ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret < 0) {
        RTE_LOG(ERR, APP, "fail to reconfigure port %u\n", port_id);
        return ret;
    }

    uint16_t nb_rxd = KNI_RX_RING_SIZE;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, NULL);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "could not adjust number of descriptors for port%u (%d)\n", (unsigned int) port_id, ret);
    }

    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(port_id, &dev_info);

    struct rte_eth_rxconf rxq_conf;
    rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = port_conf.rxmode.offloads;

    ret = rte_eth_rx_queue_setup(port_id, 0, nb_rxd, rte_eth_dev_socket_id(port_id), &rxq_conf, mbuf_pool);
    if (ret < 0) {
        RTE_LOG(ERR, APP, "fail to setup rx queue of port %u\n", port_id);
        return ret;
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        RTE_LOG(ERR, APP, "fail to restart port %u\n", port_id);
        return ret;
    }

    return 0;
}

static int kni_config_network_interface(uint16_t port_id, uint8_t if_up) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        RTE_LOG(ERR, APP, "invalid port id %u\n", port_id);
        return -EINVAL;
    }

    RTE_LOG(INFO, APP, "kni config network interface of %u %s\n", port_id, if_up ? "up" : "down");

    return 0;
}

static int kni_config_mac_address(uint16_t port_id, uint8_t mac_addr[]) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        RTE_LOG(ERR, APP, "invalid port id %u\n", port_id);
        return -EINVAL;
    }

    RTE_LOG(INFO, APP, "configure mac address of %u\n", port_id);

    int ret = rte_eth_dev_default_mac_addr_set(port_id, (struct ether_addr *) mac_addr);
    if (ret < 0) {
        RTE_LOG(ERR, APP, "failed to config mac_addr for port %u\n", port_id);
    }

    return ret;
}

static int kni_alloc(uint16_t port_id) {
    struct rte_kni_conf conf;
    memset(&conf, 0, sizeof(conf));

    // conf.core_id: lcore_kthread
    conf.core_id = 0;
    conf.force_bind = 1;
    snprintf(conf.name, RTE_KNI_NAMESIZE, "veth%u", port_id);
    conf.group_id = port_id;
    conf.mbuf_size = KNI_MAX_PACKET_SIZE;

    struct rte_eth_dev_info dev_info;
    memset(&dev_info, 0, sizeof(dev_info));
    rte_eth_dev_info_get(port_id, &dev_info);
    if (dev_info.device) {
        struct rte_bus *bus = rte_bus_find_by_device(dev_info.device);
        if (bus && !strcmp(bus->name, "pci")) {
            struct rte_pci_device *pci_dev = RTE_DEV_TO_PCI(dev_info.device);
            conf.addr = pci_dev->addr;
            conf.id = pci_dev->id;
        }
    }

    rte_eth_macaddr_get(port_id, (struct ether_addr *) &conf.mac_addr);
    rte_eth_dev_get_mtu(port_id, &conf.mtu);

    struct rte_kni_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.port_id = port_id;
    ops.change_mtu = kni_change_mtu;
    ops.config_network_if = kni_config_network_interface;
    ops.config_mac_address = kni_config_mac_address;

    kni = rte_kni_alloc(mbuf_pool, &conf, &ops);
    if (!kni) {
        rte_exit(EXIT_FAILURE, "fail to create kni for port: %u\n", port_id);
    }

    return 0;
}
// ==========KNI END==========

// ==========DPDK BEGIN==========
double cost_time_ms(struct timeval time_begin, struct timeval time_end) {
    double time_begin_us = time_begin.tv_sec * 1000000 + time_begin.tv_usec;
    double time_end_us = time_end.tv_sec * 1000000 + time_end.tv_usec;
    return (time_end_us - time_begin_us) / 1000;
}

struct rte_eth_stats old_stats;

// 统计每秒收发包信息
static void print_second_stats(void) {
    struct rte_eth_stats stats;
    rte_eth_stats_get(used_port_id, &stats);
    printf("speed rx:%10"
    PRIu64
    " (pps), tx:%10"
    PRIu64
    " (pps), drop:%10"
    PRIu64
    " (pps), rx:%20"
    PRIu64
    " (bit/s), tx:%20"
    PRIu64
    " (bit/s)\n",
            stats.ipackets - old_stats.ipackets,
            stats.opackets - old_stats.opackets,
            stats.imissed - old_stats.imissed,
            stats.ibytes - old_stats.ibytes,
            stats.obytes - old_stats.obytes);
    old_stats = stats;
}

// 统计总共收发包信息
static void print_total_stats(void) {
    struct rte_eth_stats stats;
    printf("total statistics for port %u\n", used_port_id);
    rte_eth_stats_get(used_port_id, &stats);
    printf("rx:%10"
    PRIu64
    " (pkt) tx:%10"
    PRIu64
    " (pkt) drop:%10"
    PRIu64
    " (pkt) rx:%20"
    PRIu64
    " (bit) tx:%20"
    PRIu64
    " (bit)\n", stats.ipackets, stats.opackets, stats.imissed, stats.ibytes, stats.obytes);
}

// 处理退出信号并终止程序
static void exit_signal_handler(void) {
    printf("exit signal received, exit...\n");
    force_quit = true;
}

// 初始化网口 配置收发队列
static inline int port_init(uint16_t port_id) {
    uint8_t nb_ports = rte_eth_dev_count();
    if (port_id < 0 || port_id >= nb_ports) {
        printf("port is not right\n");
        return -1;
    }

    struct rte_eth_conf port_conf = port_conf_default;
    port_conf.rxmode.max_rx_pkt_len = ETHER_MAX_LEN;

    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(port_id, &dev_info);
    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE) {
        port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
    }

    const uint16_t nb_rx_queues = RX_QUEUE;
    const uint16_t nb_tx_queues = TX_QUEUE;
    int ret;
    uint16_t q;

    // 配置设备
    ret = rte_eth_dev_configure(port_id, nb_rx_queues, nb_tx_queues, &port_conf);
    if (ret != 0) {
        printf("rte_eth_dev_configure failed\n");
        return ret;
    }

    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);

    // 配置收包队列
    for (q = 0; q < nb_rx_queues; q++) {
        ret = rte_eth_rx_queue_setup(port_id, q, nb_rxd, rte_eth_dev_socket_id(port_id), NULL, mbuf_pool);
        if (ret < 0) {
            printf("rte_eth_rx_queue_setup failed\n");
            return ret;
        }
    }

    // 配置发包队列
    for (q = 0; q < nb_tx_queues; q++) {
        ret = rte_eth_tx_queue_setup(port_id, q, nb_txd, rte_eth_dev_socket_id(port_id), NULL);
        if (ret < 0) {
            printf("rte_eth_tx_queue_setup failed\n");
            return ret;
        }
    }

    // 启动设备
    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        printf("rte_eth_dev_start failed\n");
        return ret;
    }

    // 开启混杂模式
    rte_eth_promiscuous_enable(port_id);

    return 0;
}

int lcore_misc(void *arg) {
    unsigned int lcore_id = rte_lcore_id();
    RTE_LOG(INFO, APP, "lcore_misc run in lcore %u\n", lcore_id);
    struct timeval time_now, time_old;
    while (!force_quit) {
        // KNI回调处理
        rte_kni_handle_request(kni);
        // 这个太耗时了 改用插入kni内核模块时指定carrier=on参数实现
//        update_kni_link_status(used_port_id);
        gettimeofday(&time_now, NULL);
        double cost_time = cost_time_ms(time_old, time_now);
        if (cost_time >= 1000.0) {
            gettimeofday(&time_old, NULL);
            print_second_stats();
        }
    }
    RTE_LOG(INFO, APP, "lcore_misc exit in lcore %u\n", lcore_id);
    return 0;
}

int lcore_rx(void *arg) {
    unsigned int lcore_id = rte_lcore_id();
    RTE_LOG(INFO, APP, "lcore_rx run in lcore %u\n", lcore_id);

    uint64_t loop_times = 0;
    struct timeval time_begin, time_end;
    while (!force_quit) {
#ifdef DEBUG
        loop_times++;
        if (loop_times % (1000 * 1000 * 10) == 0) {
            gettimeofday(&time_begin, NULL);
        }
#endif
        // 接收多个网卡数据帧
        struct rte_mbuf *bufs_recv[BURST_SIZE];
        uint16_t nb_rx = rte_eth_rx_burst(used_port_id, 0, bufs_recv, BURST_SIZE);

        // 有网卡接收数据
        if (likely(nb_rx != 0)) {
            struct rte_mbuf *bufs_recv_to_kni[BURST_SIZE];
            uint16_t nb_rx_to_kni = 0;
            for (int i = 0; i < nb_rx; i++) {
                uint8_t *pktbuf = rte_pktmbuf_mtod(bufs_recv[i], uint8_t * );
#ifdef DEBUG
                // 打印网卡接收到的原始数据
                printf("interface recv, len: %d, data: ", bufs_recv[i]->data_len);
                for (int j = 0; j < bufs_recv[i]->data_len; j++) {
                    printf("%02x ", pktbuf[j]);
                }
                printf("\n\n");
#endif
#ifdef KNI_BYPASS_TCP_UDP
                bool is_kni_data = false;
                if (unlikely(bufs_recv[i]->data_len < 14)) {
                    printf("error ether package\n");
                    continue;
                }
                if (likely(pktbuf[12] == 0x08) && likely(pktbuf[13] == 0x00)) {
                    // IP报文
                    if (unlikely(bufs_recv[i]->data_len < 34)) {
                        printf("error ip package\n");
                        continue;
                    }
                    if (likely(pktbuf[23] == 0x06) || likely(pktbuf[23] == 0x11)) {
                        // TCP或UDP报文
                        // 环状缓冲区数据发送
                        uint16_t ring_send_len = bufs_recv[i]->data_len;
                        write_recv_mem(pktbuf, ring_send_len);
                        rte_pktmbuf_free(bufs_recv[i]);
                    } else {
                        // 非TCP或UDP报文
                        is_kni_data = true;
                    }
                } else {
                    // 非IP报文
                    is_kni_data = true;
                }
                if (unlikely(is_kni_data)) {
                    // KNI数据
                    bufs_recv_to_kni[nb_rx_to_kni] = bufs_recv[i];
                    nb_rx_to_kni++;
                }
#else
                // 环状缓冲区数据发送
                uint16_t ring_send_len = bufs_recv[i]->data_len;
                write_recv_mem(pktbuf, ring_send_len);
                rte_pktmbuf_free(bufs_recv[i]);
#endif
            }
            // KNI数据发送
            uint16_t nb_kni_tx = rte_kni_tx_burst(kni, bufs_recv_to_kni, nb_rx_to_kni);
            if (unlikely(nb_kni_tx < nb_rx_to_kni)) {
                // 把没发送成功的mbuf释放掉
                for (int i = nb_kni_tx; i < nb_rx_to_kni; i++) {
                    rte_pktmbuf_free(bufs_recv_to_kni[i]);
                }
            }
        }
#ifdef DEBUG
        if (loop_times % (1000 * 1000 * 10) == 0) {
            gettimeofday(&time_end, NULL);
            double cost_time = cost_time_ms(time_begin, time_end);
            printf("total cost: %f ms\n", cost_time);
        }
#endif
    }

    RTE_LOG(INFO, APP, "lcore_rx exit in lcore %u\n", lcore_id);
    return 0;
}

int lcore_tx(void *arg) {
    unsigned int lcore_id = rte_lcore_id();
    RTE_LOG(INFO, APP, "lcore_tx run in lcore %u\n", lcore_id);

    uint64_t loop_times = 0;
    struct timeval time_begin, time_end;
    while (!force_quit) {
#ifdef DEBUG
        loop_times++;
        if (loop_times % (1000 * 1000 * 10) == 0) {
            gettimeofday(&time_begin, NULL);
        }
#endif
        // 环状缓冲区数据接收
        uint8_t ring_recv_buf[BURST_SIZE][1514];
        uint16_t ring_recv_buf_len[BURST_SIZE];
        int ring_recv_size = 0;
        for (int i = 0; i < BURST_SIZE; i++) {
            read_send_mem(ring_recv_buf[i], &ring_recv_buf_len[i]);
            if (unlikely(ring_recv_buf_len[i] == 0)) {
                break;
            }
            ring_recv_size++;
        }

        // KNI数据接收
        struct rte_mbuf *bufs_kni_recv[BURST_SIZE];
        uint16_t nb_kni_rx = rte_kni_rx_burst(kni, bufs_kni_recv, BURST_SIZE);

        // 有环状缓冲区数据
        if (likely(ring_recv_size > 0)) {
#ifdef DEBUG
            // 打印环状缓冲区数据
            for (int i = 0; i < ring_recv_size; i++) {
                printf("ring recv len: %d, data: ", ring_recv_buf_len[i]);
                for (int j = 0; j < ring_recv_buf_len[i]; j++) {
                    printf("%02x ", ring_recv_buf[i][j]);
                }
                printf("\n\n");
            }
#endif

            struct rte_mbuf *bufs_send[BURST_SIZE];

            // 数据拷贝
            for (int i = 0; i < ring_recv_size; i++) {
                bufs_send[i] = rte_pktmbuf_alloc(mbuf_pool);
                uint8_t *send_data = (uint8_t *) rte_pktmbuf_append(bufs_send[i],
                                                                    ring_recv_buf_len[i] * sizeof(uint8_t));
                memcpy(send_data, ring_recv_buf[i], ring_recv_buf_len[i]);
            }

            // 发送多个网卡数据帧
            uint16_t nb_tx = rte_eth_tx_burst(used_port_id, 0, bufs_send, ring_recv_size);

            if (unlikely(nb_tx < ring_recv_size)) {
                // 把没发送成功的mbuf释放掉
                for (int i = nb_tx; i < ring_recv_size; i++) {
                    rte_pktmbuf_free(bufs_send[i]);
                }
            }
        }

        // 有KNI数据
        if (likely(nb_kni_rx > 0)) {
#ifdef DEBUG
            // 打印KNI数据
            for (int i = 0; i < nb_kni_rx; i++) {
                printf("kni recv len: %d, data: ", bufs_kni_recv[i]->data_len);
                for (int j = 0; j < bufs_kni_recv[i]->data_len; j++) {
                    uint8_t *pktbuf = rte_pktmbuf_mtod(bufs_kni_recv[i], uint8_t * );
                    printf("%02x ", pktbuf[j]);
                }
                printf("\n\n");
            }
#endif
            // 发送多个网卡数据帧
            uint16_t nb_tx = rte_eth_tx_burst(used_port_id, 0, bufs_kni_recv, nb_kni_rx);
            if (unlikely(nb_tx < nb_kni_rx)) {
                // 把没发送成功的mbuf释放掉
                for (int i = nb_tx; i < nb_kni_rx; i++) {
                    rte_pktmbuf_free(bufs_kni_recv[i]);
                }
            }
        }
#ifdef DEBUG
        if (loop_times % (1000 * 1000 * 10) == 0) {
            gettimeofday(&time_end, NULL);
            double cost_time = cost_time_ms(time_begin, time_end);
            printf("total cost: %f ms\n", cost_time);
        }
#endif
    }

    RTE_LOG(INFO, APP, "lcore_tx exit in lcore %u\n", lcore_id);
    return 0;
}

int lcore_rx_tx(void *arg) {
    unsigned int lcore_id = rte_lcore_id();
    RTE_LOG(INFO, APP, "lcore_rx_tx run in lcore %u\n", lcore_id);

    uint64_t loop_times = 0;
    struct timeval time_begin, time_end;
    while (!force_quit) {
#ifdef DEBUG
        loop_times++;
        if (loop_times % (1000 * 1000 * 10) == 0) {
            gettimeofday(&time_begin, NULL);
        }
#endif
        // KNI回调处理
        rte_kni_handle_request(kni);
        // 这个太耗时了 改用插入kni内核模块时指定carrier=on参数实现
//        update_kni_link_status(used_port_id);

        // 环状缓冲区数据接收
        uint8_t ring_recv_buf[BURST_SIZE][1514];
        uint16_t ring_recv_buf_len[BURST_SIZE];
        int ring_recv_size = 0;
        for (int i = 0; i < BURST_SIZE; i++) {
            read_send_mem(ring_recv_buf[i], &ring_recv_buf_len[i]);
            if (unlikely(ring_recv_buf_len[i] == 0)) {
                break;
            }
            ring_recv_size++;
        }

        // KNI数据接收
        struct rte_mbuf *bufs_kni_recv[BURST_SIZE];
        uint16_t nb_kni_rx = rte_kni_rx_burst(kni, bufs_kni_recv, BURST_SIZE);

        // 接收多个网卡数据帧
        struct rte_mbuf *bufs_recv[BURST_SIZE];
        uint16_t nb_rx = rte_eth_rx_burst(used_port_id, 0, bufs_recv, BURST_SIZE);

#ifdef IDEL_SLEEP
        // 无包时短暂睡眠节省CPU资源
        if (unlikely(nb_rx == 0) && unlikely(ring_recv_size == 0) && unlikely(nb_kni_rx == 0)) {
            usleep(1000 * 100);
            continue;
        }
#endif

        // 有环状缓冲区数据
        if (likely(ring_recv_size > 0)) {
#ifdef DEBUG
            // 打印环状缓冲区数据
            for (int i = 0; i < ring_recv_size; i++) {
                printf("ring recv len: %d, data: ", ring_recv_buf_len[i]);
                for (int j = 0; j < ring_recv_buf_len[i]; j++) {
                    printf("%02x ", ring_recv_buf[i][j]);
                }
                printf("\n\n");
            }
#endif

            struct rte_mbuf *bufs_send[BURST_SIZE];

            // 数据拷贝
            for (int i = 0; i < ring_recv_size; i++) {
                bufs_send[i] = rte_pktmbuf_alloc(mbuf_pool);
                uint8_t *send_data = (uint8_t *) rte_pktmbuf_append(bufs_send[i],
                                                                    ring_recv_buf_len[i] * sizeof(uint8_t));
                memcpy(send_data, ring_recv_buf[i], ring_recv_buf_len[i]);
            }

            // 发送多个网卡数据帧
            uint16_t nb_tx = rte_eth_tx_burst(used_port_id, 0, bufs_send, ring_recv_size);

            if (unlikely(nb_tx < ring_recv_size)) {
                // 把没发送成功的mbuf释放掉
                for (int i = nb_tx; i < ring_recv_size; i++) {
                    rte_pktmbuf_free(bufs_send[i]);
                }
            }
        }

        // 有KNI数据
        if (likely(nb_kni_rx > 0)) {
#ifdef DEBUG
            // 打印KNI数据
            for (int i = 0; i < nb_kni_rx; i++) {
                printf("kni recv len: %d, data: ", bufs_kni_recv[i]->data_len);
                for (int j = 0; j < bufs_kni_recv[i]->data_len; j++) {
                    uint8_t *pktbuf = rte_pktmbuf_mtod(bufs_kni_recv[i], uint8_t * );
                    printf("%02x ", pktbuf[j]);
                }
                printf("\n\n");
            }
#endif
            // 发送多个网卡数据帧
            uint16_t nb_tx = rte_eth_tx_burst(used_port_id, 0, bufs_kni_recv, nb_kni_rx);
            if (unlikely(nb_tx < nb_kni_rx)) {
                // 把没发送成功的mbuf释放掉
                for (int i = nb_tx; i < nb_kni_rx; i++) {
                    rte_pktmbuf_free(bufs_kni_recv[i]);
                }
            }
        }

        // 有网卡接收数据
        if (likely(nb_rx != 0)) {
            struct rte_mbuf *bufs_recv_to_kni[BURST_SIZE];
            uint16_t nb_rx_to_kni = 0;
            for (int i = 0; i < nb_rx; i++) {
                uint8_t *pktbuf = rte_pktmbuf_mtod(bufs_recv[i], uint8_t * );
#ifdef DEBUG
                // 打印网卡接收到的原始数据
                printf("interface recv, len: %d, data: ", bufs_recv[i]->data_len);
                for (int j = 0; j < bufs_recv[i]->data_len; j++) {
                    printf("%02x ", pktbuf[j]);
                }
                printf("\n\n");
#endif
#ifdef KNI_BYPASS_TCP_UDP
                bool is_kni_data = false;
                if (unlikely(bufs_recv[i]->data_len < 14)) {
                    printf("error ether package\n");
                    continue;
                }
                if (likely(pktbuf[12] == 0x08) && likely(pktbuf[13] == 0x00)) {
                    // IP报文
                    if (unlikely(bufs_recv[i]->data_len < 34)) {
                        printf("error ip package\n");
                        continue;
                    }
                    if (likely(pktbuf[23] == 0x06) || likely(pktbuf[23] == 0x11)) {
                        // TCP或UDP报文
                        // 环状缓冲区数据发送
                        uint16_t ring_send_len = bufs_recv[i]->data_len;
                        write_recv_mem(pktbuf, ring_send_len);
                        rte_pktmbuf_free(bufs_recv[i]);
                    } else {
                        // 非TCP或UDP报文
                        is_kni_data = true;
                    }
                } else {
                    // 非IP报文
                    is_kni_data = true;
                }
                if (unlikely(is_kni_data)) {
                    // KNI数据
                    bufs_recv_to_kni[nb_rx_to_kni] = bufs_recv[i];
                    nb_rx_to_kni++;
                }
#else
                // 环状缓冲区数据发送
                uint16_t ring_send_len = bufs_recv[i]->data_len;
                write_recv_mem(pktbuf, ring_send_len);
                rte_pktmbuf_free(bufs_recv[i]);
#endif
            }
            // KNI数据发送
            uint16_t nb_kni_tx = rte_kni_tx_burst(kni, bufs_recv_to_kni, nb_rx_to_kni);
            if (unlikely(nb_kni_tx < nb_rx_to_kni)) {
                // 把没发送成功的mbuf释放掉
                for (int i = nb_kni_tx; i < nb_rx_to_kni; i++) {
                    rte_pktmbuf_free(bufs_recv_to_kni[i]);
                }
            }
        }
#ifdef DEBUG
        if (loop_times % (1000 * 1000 * 10) == 0) {
            gettimeofday(&time_end, NULL);
            double cost_time = cost_time_ms(time_begin, time_end);
            printf("total cost: %f ms\n", cost_time);
        }
#endif
    }

    RTE_LOG(INFO, APP, "lcore_rx_tx exit in lcore %u\n", lcore_id);
    return 0;
}

void parse_args(char *args_raw, int *argc, char ***argv) {
    int args_raw_len = strlen(args_raw);
    char *args = (char *) malloc(args_raw_len + 2);
    memcpy(args, args_raw, args_raw_len);
    args[args_raw_len] = ' ';
    args[args_raw_len + 1] = 0x00;
    (*argc) = 0;
    int args_len = strlen(args);
    for (int i = 0; i < args_len; i++) {
        if (args[i] == ' ') {
            (*argc)++;
        }
    }
    (*argv) = (char **) malloc(sizeof(char *) * (*argc));
    char **argv_copy = (*argv);
    int head = 0;
    for (int i = 0; i < args_len; i++) {
        if (args[i] == ' ') {
            int arg_len = i - head;
            *argv_copy = (char *) malloc(arg_len + 1);
            memset(*argv_copy, 0x00, arg_len + 1);
            memcpy(*argv_copy, args + head, arg_len);
            head = i + 1;
            argv_copy++;
        }
    }
}

int dpdk_main(char *args) {
    printf("dpdk start, version: %s\n", VERSION);
    printf("args: %s\n", args);
    int argc = 0;
    char **argv = NULL;
    parse_args(args, &argc, &argv);
    printf("argc: %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("argv: %s\n", argv[i]);
    }

    printf("\n");

    // 初始化DPDK
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "eal init failed\n");
    }
    argc -= ret;
    argv += ret;
    printf("\n");

    argc--;
    argv++;
    printf("test arg: %s\n", *argv);

    force_quit = false;

    uint8_t nb_ports = rte_eth_dev_count();
    for (int i = 0; i < nb_ports; i++) {
        char dev_name[RTE_DEV_NAME_MAX_LEN];
        rte_eth_dev_get_name_by_port(i, dev_name);
        printf("port number: %d, port pci: %s, ", i, dev_name);
        struct ether_addr dev_eth_addr;
        rte_eth_macaddr_get(i, &dev_eth_addr);
        printf("mac address: %02X:%02X:%02X:%02X:%02X:%02X\n",
               dev_eth_addr.addr_bytes[0],
               dev_eth_addr.addr_bytes[1],
               dev_eth_addr.addr_bytes[2],
               dev_eth_addr.addr_bytes[3],
               dev_eth_addr.addr_bytes[4],
               dev_eth_addr.addr_bytes[5]);
    }

    printf("choose port id: %u\n", used_port_id);

    // 申请mbuf内存池
    mbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                        rte_socket_id());
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "mbuf pool create failed\n");
    }

    // 网口初始化
    if (port_init(used_port_id) != 0) {
        rte_exit(EXIT_FAILURE, "port init failed\n");
    }
    printf("\n");

    // ==========KNI==========
    rte_kni_init(1);
    kni_alloc(used_port_id);
    // ==========KNI==========

#ifdef SINGLE_CORE
    // 单核心机器上无法启动多线程 暂时用主线程跑
    lcore_rx_tx(NULL);
#else
    // 线程核心绑定 循环处理数据包
    rte_eal_remote_launch(lcore_misc, NULL, 1);
    rte_eal_remote_launch(lcore_tx, NULL, 2);
    rte_eal_remote_launch(lcore_rx, NULL, 3);
    rte_eal_mp_wait_lcore();
#endif

    print_total_stats();

    if (rte_kni_release(kni)) {
        printf("fail to release kni\n");
    }
    rte_eth_dev_stop(used_port_id);

    return 0;
}
// ==========DPDK END==========

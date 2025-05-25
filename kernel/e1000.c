#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //

  acquire(&e1000_lock);// 加锁

  uint32 idx = regs[E1000_TDT]; // 获取下一个可用索引,从网卡寄存器中拿到尾指针
  struct tx_desc *desc = &tx_ring[idx]; // 获取当前描述符，获取尾巴处的描述符

  if((desc ->status & E1000_TXD_STAT_DD) == 0) { // 还没完成之前的传输请求，看看这个指针处是否空闲
    release(&e1000_lock); // 释放锁
    return -1; // TX descriptor not ready
  }

  if(tx_mbufs[idx]) { // 释放之前的mbuf，说明这个位置之前分配过数据
    mbuffree(tx_mbufs[idx]);
    tx_mbufs[idx] = 0;
  }
  // 设置描述符的一些信息
  desc->addr = (uint64)m->head; // 设置描述符地址， 设置网卡 DMA 从哪里读取数据
  desc->length = m->len; // 设置描述符长度，以太网帧长度
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS; // 设置命令
  // 设置完，即可等待硬件发送
  tx_mbufs[idx] = m; // 保存mbuf指针，也就是保存这个数据包的指针，网卡通过desc描述符，找到对应的缓冲区位置，然后开始发送buf数据。

  // 更新发送描述符尾指针
  regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE; // 更新发送描述符尾指针，循环队列
  
  release(&e1000_lock); // 释放锁
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //

  // 循环检查接收描述符中的数据包
  while(1) {
    uint32 idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE; // 获取下一个接收描述符索引
    struct rx_desc *desc = &rx_ring[idx]; // 获取当前接收描述符

    if((desc->status & E1000_RXD_STAT_DD) == 0) { // 如果没有数据包到达
      break; // 退出循环
    }

    rx_mbufs[idx]->len = desc->length; // 设置mbuf长度，DMA最终会将数据包放在mbuf中

    net_rx(rx_mbufs[idx]); // 处理接收到的mbuf

    rx_mbufs[idx] = mbufalloc(0); // 重置mbuf长度
    desc->addr = (uint64)rx_mbufs[idx]->head; // 设置接收描述符地址
    desc->status = 0; // 设置接收描述符长度

    regs[E1000_RDT] = idx; // 更新接收描述符尾指针
  }



}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}

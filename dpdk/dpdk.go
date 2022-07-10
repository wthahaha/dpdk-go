package dpdk

import (
	"fmt"
	"os"
	"reflect"
	"time"
	"unsafe"
)

// #cgo CFLAGS: -I/root/dpdk-stable-18.11.11/x86_64-native-linuxapp-gcc/include -msse4.2
// #cgo LDFLAGS: -L/root/dpdk-stable-18.11.11/x86_64-native-linuxapp-gcc/lib -Wl,--whole-archive -ldpdk -Wl,--no-whole-archive -ldl -pthread -lnuma -lm
// #include "./cpp/dpdk.c"
import "C"

const RING_BUFFER_SIZE uintptr = 134217728
const DEBUG bool = false

var DPDK_RX_CHAN = make(chan []byte, 1048576)
var DPDK_TX_CHAN = make(chan []byte, 1048576)

var mem_send_head unsafe.Pointer = nil
var mem_send_cur unsafe.Pointer = nil
var mem_recv_head unsafe.Pointer = nil
var mem_recv_cur unsafe.Pointer = nil
var recv_pos_pointer_addr *unsafe.Pointer = nil
var send_pos_pointer_addr *unsafe.Pointer = nil

func Alloc() {
	mem_send_head = C.alloc_send_mem()
	mem_recv_head = C.alloc_recv_mem()
	recv_pos_pointer_addr = C.recv_pos_pointer_addr()
	send_pos_pointer_addr = C.send_pos_pointer_addr()
}

func Run() {
	go run_dpdk()
}

func Exit() {
	C.exit_signal_handler()
}

func Handle() {
	go tx_pkt()
	go rx_pkt()
	if DEBUG {
		go print_mem()
	}
}

func Loopback() {
	go func() {
		pkt_rx_buf := make([]uint8, 1514)
		pkt_len := uint16(0)
		for {
			read_recv_mem(pkt_rx_buf, &pkt_len)
			if pkt_len == 0 {
				// 单个CPU核心轮询
				continue
			}
			pkt := pkt_rx_buf[:pkt_len]
			if DEBUG {
				fmt.Printf("loopback pkt, len: %v, data: %02x\n", pkt_len, pkt)
			}
			write_send_mem(pkt, pkt_len)
		}
	}()
}

func rx_pkt() {
	pkt_rx_buf := make([]uint8, 1514)
	pkt_len := uint16(0)
	for {
		read_recv_mem(pkt_rx_buf, &pkt_len)
		if pkt_len == 0 {
			// 单个CPU核心轮询
			continue
		}
		pkt := pkt_rx_buf[:pkt_len]
		DPDK_RX_CHAN <- pkt
		if DEBUG {
			fmt.Printf("rx pkt, len: %v, data: %02x\n", pkt_len, pkt)
		}
	}
}

func tx_pkt() {
	for {
		pkt := <-DPDK_TX_CHAN
		pkt_len := len(pkt)
		if DEBUG {
			fmt.Printf("tx pkt, len: %v, data: %02x\n", pkt_len, pkt)
		}
		write_send_mem(pkt, uint16(pkt_len))
	}
}

func print_mem() {
	go func() {
		for {
			time.Sleep(time.Second * 1)
			fmt.Printf("\n++++++++++ mem recv ++++++++++\n")
			for offset := uintptr(0); offset < RING_BUFFER_SIZE; offset++ {
				byte_data := (*uint8)(unsafe.Pointer(uintptr(mem_recv_head) + offset))
				fmt.Printf("%02x ", *byte_data)
			}
			fmt.Printf("\n++++++++++ mem recv ++++++++++\n")
		}
	}()
	go func() {
		for {
			time.Sleep(time.Second * 1)
			fmt.Printf("\n++++++++++ mem send ++++++++++\n")
			for offset := uintptr(0); offset < RING_BUFFER_SIZE; offset++ {
				byte_data := (*uint8)(unsafe.Pointer(uintptr(mem_send_head) + offset))
				fmt.Printf("%02x ", *byte_data)
			}
			fmt.Printf("\n++++++++++ mem send ++++++++++\n")
		}
	}()
}

func build_cpp_main_args(argList []string) *C.char {
	cmd := ""
	for i, v := range argList {
		cmd += v
		if i < len(argList)-1 {
			cmd += " "
		}
	}
	args := C.CString(cmd)
	return args
}

func run_dpdk() {
	args := build_cpp_main_args([]string{os.Args[0], "-l", "0-3", "-n", "1", "--", "test_arg"})
	C.dpdk_main(args)
	C.free(unsafe.Pointer(args))
}

// 环状缓冲区写入
func write_send_mem_core(data [1520]uint8, len uint16) uint8 {
	// 内存对齐
	aling_size := len % 4
	if aling_size != 0 {
		aling_size = 4 - aling_size
	}
	len += aling_size
	head_u32 := uint32(0)
	head_u32 = ((uint32(data[0])) << 0) +
		((uint32(data[1])) << 8) +
		((uint32(data[2])) << 16) +
		((uint32(data[3])) << 24)
	overflow := int32((uintptr(mem_send_cur) + uintptr(len)) - (uintptr(mem_send_head) + RING_BUFFER_SIZE))
	if DEBUG {
		fmt.Printf("[write_send_mem_core] overflow: %d, len: %d, mem_send_cur: %p, mem_send_head: %p, send_pos_pointer: %p\n",
			overflow, len, mem_send_cur, mem_send_head, *send_pos_pointer_addr)
	}
	if overflow >= 0 {
		// 有溢出
		if uintptr(mem_send_cur) < uintptr(*send_pos_pointer_addr) {
			// 已经处于读写指针交叉状态 丢弃数据
			return 1
		}
		if uintptr(overflow) >= (uintptr(*send_pos_pointer_addr) - uintptr(mem_send_head)) {
			// 即使进入读写指针交叉状态 剩余内存依然不足 丢弃数据
			return 1
		}
		head_ptr := (*uint32)(mem_send_cur)
		// 写入头部 原子操作
		*head_ptr = head_u32
		if (int32(len) - overflow) > 4 {
			// 拷贝前半段数据
			{
				internel_slice_ptr := new(reflect.SliceHeader)
				internel_slice_ptr.Data = uintptr(mem_send_cur) + uintptr(4)
				internel_slice_ptr.Len = int((int32(len) - overflow) - 4)
				internel_slice_ptr.Cap = int((int32(len) - overflow) - 4)
				slice_ptr := *(*[]uint8)(unsafe.Pointer(internel_slice_ptr))
				copy(slice_ptr, data[4:])
			}
		}
		mem_send_cur = mem_send_head
		// 拷贝后半段数据
		{
			internel_slice_ptr := new(reflect.SliceHeader)
			internel_slice_ptr.Data = uintptr(mem_send_cur)
			internel_slice_ptr.Len = int(overflow)
			internel_slice_ptr.Cap = int(overflow)
			slice_ptr := *(*[]uint8)(unsafe.Pointer(internel_slice_ptr))
			copy(slice_ptr, data[4+((int32(len)-overflow)-4):])
		}
		mem_send_cur = unsafe.Pointer(uintptr(mem_send_cur) + uintptr(overflow))
		if uintptr(mem_send_cur) >= uintptr(mem_send_head)+RING_BUFFER_SIZE {
			mem_send_cur = mem_send_head
		}
	} else {
		// 无溢出
		if (uintptr(mem_send_cur) < uintptr(*send_pos_pointer_addr)) && ((uintptr(mem_send_cur) + uintptr(len)) >= uintptr(*send_pos_pointer_addr)) {
			// 读写指针交叉状态下剩余内存不足 丢弃数据
			return 1
		}
		head_ptr := (*uint32)(mem_send_cur)
		// 写入头部 原子操作
		*head_ptr = head_u32
		{
			internel_slice_ptr := new(reflect.SliceHeader)
			internel_slice_ptr.Data = uintptr(mem_send_cur) + uintptr(4)
			internel_slice_ptr.Len = int(len - 4)
			internel_slice_ptr.Cap = int(len - 4)
			slice_ptr := *(*[]uint8)(unsafe.Pointer(internel_slice_ptr))
			copy(slice_ptr, data[4:])
		}
		mem_send_cur = unsafe.Pointer(uintptr(mem_send_cur) + uintptr(len))
		if uintptr(mem_send_cur) >= uintptr(mem_send_head)+RING_BUFFER_SIZE {
			mem_send_cur = mem_send_head
		}
	}
	return 0
}

// 写入发送缓冲区
func write_send_mem(data []uint8, len uint16) {
	if mem_send_cur == nil {
		mem_send_cur = mem_send_head
	}
	if len > 1514 {
		return
	}
	// 4字节头部 + 最大1514字节数据 + 2字节内存对齐
	data_pkg := [4 + 1514 + 2]uint8{0x00}
	// 数据长度标识
	data_pkg[0] = uint8(len)
	data_pkg[1] = uint8(len >> 8)
	// 写入完成标识
	finish_flag_pointer := (*uint8)(unsafe.Pointer(uintptr(mem_send_cur) + uintptr(2)))
	data_pkg[2] = 0x00
	// 内存对齐
	data_pkg[3] = 0x00
	// 写入数据
	copy(data_pkg[4:], data[:len])
	ret := write_send_mem_core(data_pkg, len+4)
	if ret == 1 {
		return
	}
	// 将后4个字节即长度标识与写入完成标识的内存置为0x00
	*(*uint8)(unsafe.Pointer(uintptr(mem_send_cur) + uintptr(0))) = 0x00
	*(*uint8)(unsafe.Pointer(uintptr(mem_send_cur) + uintptr(1))) = 0x00
	*(*uint8)(unsafe.Pointer(uintptr(mem_send_cur) + uintptr(2))) = 0x00
	*(*uint8)(unsafe.Pointer(uintptr(mem_send_cur) + uintptr(3))) = 0x00
	// 修改写入完成标识 原子操作
	*finish_flag_pointer = 0x01
}

// 读取接收缓冲区
func read_recv_mem(data []uint8, len *uint16) {
	if mem_recv_cur == nil {
		mem_recv_cur = mem_recv_head
	}
	*len = 0
	// 读取头部 原子操作
	head_ptr := (*uint32)(mem_recv_cur)
	head_u32 := *head_ptr
	*len = uint16(head_u32)
	if *len == 0 {
		// 没有新数据
		return
	}
	finish_flag := uint8(head_u32 >> 16)
	if finish_flag == 0x00 {
		// 数据尚未写入完成
		*len = 0
		return
	}
	mem_recv_cur = unsafe.Pointer(uintptr(mem_recv_cur) + uintptr(4))
	if uintptr(mem_recv_cur) >= uintptr(mem_recv_head)+RING_BUFFER_SIZE {
		mem_recv_cur = mem_recv_head
	}
	overflow := int32((uintptr(mem_recv_cur) + uintptr(*len)) - (uintptr(mem_recv_head) + RING_BUFFER_SIZE))
	if DEBUG {
		fmt.Printf("[read_recv_mem] overflow: %d, len: %d, mem_recv_cur: %p, mem_recv_head: %p, recv_pos_pointer: %p\n",
			overflow, *len, mem_recv_cur, mem_recv_head, *recv_pos_pointer_addr)
	}
	if overflow >= 0 {
		// 拷贝前半段数据
		{
			internel_slice_ptr := new(reflect.SliceHeader)
			internel_slice_ptr.Data = uintptr(mem_recv_cur)
			internel_slice_ptr.Len = int(int32(*len) - overflow)
			internel_slice_ptr.Cap = int(int32(*len) - overflow)
			slice_ptr := *(*[]uint8)(unsafe.Pointer(internel_slice_ptr))
			copy(data, slice_ptr)
		}
		mem_recv_cur = mem_recv_head
		// 拷贝后半段数据
		{
			internel_slice_ptr := new(reflect.SliceHeader)
			internel_slice_ptr.Data = uintptr(mem_recv_cur)
			internel_slice_ptr.Len = int(int32(*len) - (int32(*len) - overflow))
			internel_slice_ptr.Cap = int(int32(*len) - (int32(*len) - overflow))
			slice_ptr := *(*[]uint8)(unsafe.Pointer(internel_slice_ptr))
			copy(data[int32(*len)-overflow:], slice_ptr)
		}
		// 内存对齐
		aling_size := overflow % 4
		if aling_size != 0 {
			aling_size = 4 - aling_size
		}
		mem_recv_cur = unsafe.Pointer(uintptr(mem_recv_cur) + uintptr(overflow) + uintptr(aling_size))
		if uintptr(mem_recv_cur) >= uintptr(mem_recv_head)+RING_BUFFER_SIZE {
			mem_recv_cur = mem_recv_head
		}
		*recv_pos_pointer_addr = mem_recv_cur
	} else {
		{
			internel_slice_ptr := new(reflect.SliceHeader)
			internel_slice_ptr.Data = uintptr(mem_recv_cur)
			internel_slice_ptr.Len = int(*len)
			internel_slice_ptr.Cap = int(*len)
			slice_ptr := *(*[]uint8)(unsafe.Pointer(internel_slice_ptr))
			copy(data, slice_ptr)
		}
		// 内存对齐
		aling_size := *len % 4
		if aling_size != 0 {
			aling_size = 4 - aling_size
		}
		mem_recv_cur = unsafe.Pointer(uintptr(mem_recv_cur) + uintptr(*len) + uintptr(aling_size))
		if uintptr(mem_recv_cur) >= uintptr(mem_recv_head)+RING_BUFFER_SIZE {
			mem_recv_cur = mem_recv_head
		}
		*recv_pos_pointer_addr = mem_recv_cur
	}
}

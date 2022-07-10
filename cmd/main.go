package main

import (
	"dpdk-go/dpdk"
	"encoding/hex"
	"fmt"
	"time"
)

func main() {
	dpdk.Alloc()
	dpdk.Run()
	// 等待DPDK启动完成
	time.Sleep(time.Second * 10)
	//dpdk.Loopback()
	dpdk.Handle()
	go func() {
		for {
			pkt := <-dpdk.DPDK_RX_CHAN
			fmt.Printf("rx pkt, len: %v, data: %v\n", len(pkt), pkt)
		}
	}()
	go func() {
		pkt, err := hex.DecodeString("112233aabbcc")
		if err != nil {
			panic(err)
		}
		for {
			dpdk.DPDK_TX_CHAN <- pkt
		}
	}()
	time.Sleep(time.Second * 30)
	dpdk.Exit()
	select {}
}

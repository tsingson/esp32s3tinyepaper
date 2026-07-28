package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"time"
)

const (
	frameMagic       = byte(0xB1)
	payloadTypeDrone = byte(1)
	frameHeaderSize  = 4
	dronePayloadSize = 65
	ioRetries        = 3
	acceptRetries    = 3
)

type frameHeader struct {
	Magic         byte
	PayloadType   byte
	PayloadLength uint16
}

func previewBytes(buf []byte, limit int) string {
	if len(buf) <= limit {
		return fmt.Sprintf("% x", buf)
	}
	return fmt.Sprintf("% x ...", buf[:limit])
}

func decodeFrameHeader(buf []byte) (frameHeader, error) {
	if len(buf) != frameHeaderSize {
		return frameHeader{}, fmt.Errorf("invalid header size %d", len(buf))
	}

	return frameHeader{
		Magic:         buf[0],
		PayloadType:   buf[1],
		PayloadLength: uint16(buf[2]) | uint16(buf[3])<<8,
	}, nil
}

func encodeFrameHeader(h frameHeader) []byte {
	return []byte{
		h.Magic,
		h.PayloadType,
		byte(h.PayloadLength),
		byte(h.PayloadLength >> 8),
	}
}

func readFull(conn net.Conn, buf []byte, timeout time.Duration) error {
	total := 0
	for attempt := 1; attempt <= ioRetries && total < len(buf); attempt++ {
		if err := conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
			return err
		}
		n, err := io.ReadFull(conn, buf[total:])
		total += n
		if err == nil {
			return nil
		}
		if netErr, ok := err.(net.Error); ok && netErr.Timeout() && attempt < ioRetries {
			log.Printf("read timeout remote=%s attempt=%d/%d bytes=%d/%d", conn.RemoteAddr(), attempt, ioRetries, total, len(buf))
			continue
		}
		return err
	}
	return fmt.Errorf("read exhausted retries bytes=%d/%d", total, len(buf))
}

func writeFull(conn net.Conn, buf []byte, timeout time.Duration) error {
	for len(buf) > 0 {
		if err := conn.SetWriteDeadline(time.Now().Add(timeout)); err != nil {
			return err
		}
		n, err := conn.Write(buf)
		if err != nil {
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				log.Printf("write timeout remote=%s remaining=%d", conn.RemoteAddr(), len(buf))
				continue
			}
			return err
		}
		buf = buf[n:]
	}
	return nil
}

func readFrame(conn net.Conn, timeout time.Duration) ([]byte, error) {
	headerBuf := make([]byte, frameHeaderSize)
	if err := readFull(conn, headerBuf, timeout); err != nil {
		return nil, fmt.Errorf("read header: %w", err)
	}

	header, err := decodeFrameHeader(headerBuf)
	if err != nil {
		return nil, err
	}
	if header.Magic != frameMagic {
		return nil, fmt.Errorf("bad magic %d", header.Magic)
	}
	if header.PayloadType != payloadTypeDrone {
		return nil, fmt.Errorf("bad payload type %d", header.PayloadType)
	}
	if header.PayloadLength != dronePayloadSize {
		return nil, fmt.Errorf("invalid payload length %d", header.PayloadLength)
	}
	log.Printf("rx header remote=%s magic=0x%02x type=%d len=%d", conn.RemoteAddr(), header.Magic, header.PayloadType, header.PayloadLength)

	payload := make([]byte, int(header.PayloadLength))
	if err := readFull(conn, payload, timeout); err != nil {
		return nil, fmt.Errorf("read payload: %w", err)
	}
	log.Printf("rx payload remote=%s bytes=%d preview=%s", conn.RemoteAddr(), len(payload), previewBytes(payload, 16))

	load := new(Drone)
	load.Decode(payload)
	if load != nil {
		log.Printf("drone: %s", load.String())

		log.Printf("gps: %f %f  ", load.Position.Longitude, load.Position.Latitude)
	}
	return payload, nil
}

func writeFrame(conn net.Conn, payload []byte, timeout time.Duration) error {
	header := frameHeader{
		Magic:         frameMagic,
		PayloadType:   payloadTypeDrone,
		PayloadLength: uint16(len(payload)),
	}
	log.Printf("tx header remote=%s magic=0x%02x type=%d len=%d", conn.RemoteAddr(), header.Magic, header.PayloadType, header.PayloadLength)
	if err := writeFull(conn, encodeFrameHeader(header), timeout); err != nil {
		return fmt.Errorf("write header: %w", err)
	}
	if err := writeFull(conn, payload, timeout); err != nil {
		return fmt.Errorf("write payload: %w", err)
	}
	log.Printf("tx payload remote=%s bytes=%d preview=%s", conn.RemoteAddr(), len(payload), previewBytes(payload, 16))
	return nil
}

func serveConnection(conn net.Conn, rounds int, timeout time.Duration) error {
	defer conn.Close()
	log.Printf("conn open remote=%s local=%s rounds=%d", conn.RemoteAddr(), conn.LocalAddr(), rounds)
	defer log.Printf("conn close remote=%s", conn.RemoteAddr())

	for i := 0; i < rounds; i++ {
		log.Printf("frame start remote=%s index=%d", conn.RemoteAddr(), i)
		payload, err := readFrame(conn, timeout)
		if err != nil {
			return fmt.Errorf("frame %d: %w", i, err)
		}

		if err := writeFrame(conn, payload, timeout); err != nil {
			return fmt.Errorf("frame %d: %w", i, err)
		}
		log.Printf("frame done remote=%s index=%d", conn.RemoteAddr(), i)
	}

	return nil
}

func main() {
	log.SetFlags(0)
	listenAddr := flag.String("listen", ":8061", "listen address")
	rounds := flag.Int("rounds", 1, "number of frames to process per connection")
	ioTimeout := flag.Duration("io-timeout", 15*time.Second, "per read/write deadline")
	flag.Parse()

	if *rounds <= 0 {
		log.Fatalf("invalid rounds %d", *rounds)
	}

	ln, err := net.Listen("tcp", *listenAddr)
	if err != nil {
		log.Fatalf("listen: %v", err)
	}

	log.Printf("listen addr=%s rounds=%d mode=continuous io_timeout=%s", ln.Addr().String(), *rounds, ioTimeout.String())
	fmt.Printf("READY %s\n", ln.Addr().String())

	for connIndex := 0; ; connIndex++ {
		var conn net.Conn
		var acceptErr error
		for attempt := 1; attempt <= acceptRetries; attempt++ {
			conn, acceptErr = ln.Accept()
			if acceptErr == nil {
				break
			}
			if netErr, ok := acceptErr.(net.Error); ok && netErr.Temporary() && attempt < acceptRetries {
				log.Printf("accept temporary error attempt=%d/%d err=%v", attempt, acceptRetries, acceptErr)
				time.Sleep(300 * time.Millisecond)
				continue
			}
			log.Fatalf("accept: %v", acceptErr)
		}
		log.Printf("accept remote=%s local=%s index=%d", conn.RemoteAddr(), conn.LocalAddr(), connIndex)

		go func(conn net.Conn) {
			if serveErr := serveConnection(conn, *rounds, *ioTimeout); serveErr != nil {
				log.Printf("serve error remote=%s err=%v", conn.RemoteAddr(), serveErr)
			}
		}(conn)
	}
}

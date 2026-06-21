package flux

import (
	"bufio"
	"encoding/json"
	"fmt"
	"hash/crc32"
	"os"
	"sync"
)

// walEvent is a single record in the write-ahead log.
// Checksum protects against silent data corruption.
type walEvent struct {
	Action     string         `json:"action"`
	Collection string         `json:"collection"`
	Metric     DistanceMetric `json:"metric,omitempty"`
	Document   *Document      `json:"document,omitempty"`
	Documents  []*Document    `json:"documents,omitempty"` // batch_upsert
	ID         string         `json:"id,omitempty"`
	IDs        []string       `json:"ids,omitempty"`       // batch_delete
	Checksum   uint32         `json:"crc32,omitempty"`     // CRC32 of all other fields
}

// walLine wraps the event with metadata for appending.
// Format: 4-byte length prefix + JSON + newline
// The checksum is computed over the JSON (excluding the checksum field itself).

type WAL struct {
	path string
	file *os.File
	mu   sync.Mutex
}

func NewWAL(path string) (*WAL, error) {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0o644)
	if err != nil {
		return nil, fmt.Errorf("open wal: %w", err)
	}
	return &WAL{path: path, file: file}, nil
}

// Append writes a record to the WAL with CRC32 checksum protection.
func (w *WAL) Append(record *walEvent) error {
	w.mu.Lock()
	defer w.mu.Unlock()

	// Compute checksum over the JSON (set Checksum=0 first)
	record.Checksum = 0
	data, err := json.Marshal(record)
	if err != nil {
		return err
	}
	record.Checksum = crc32.ChecksumIEEE(data)

	// Re-marshal with checksum
	dataWithCRC, err := json.Marshal(record)
	if err != nil {
		return err
	}

	if _, err := w.file.Write(append(dataWithCRC, '\n')); err != nil {
		return fmt.Errorf("wal append: %w", err)
	}
	if err := w.file.Sync(); err != nil {
		return fmt.Errorf("wal sync: %w", err)
	}
	return nil
}

func (w *WAL) Replay(handler func(record *walEvent) error) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if _, err := w.file.Seek(0, 0); err != nil {
		return fmt.Errorf("wal seek: %w", err)
	}

	scanner := bufio.NewScanner(w.file)
	// Increase buffer for large batch events
	scanner.Buffer(make([]byte, 1024*1024), 1024*1024)

	lineNumber := 0
	for scanner.Scan() {
		lineNumber++
		raw := scanner.Bytes()
		if len(raw) == 0 {
			continue
		}

		// Peek at checksum for verification
		var checkRecord struct {
			Checksum uint32 `json:"crc32"`
		}
		if err := json.Unmarshal(raw, &checkRecord); err != nil {
			return fmt.Errorf("wal replay line %d: %w", lineNumber, err)
		}

		// Decode full record
		var record walEvent
		if err := json.Unmarshal(raw, &record); err != nil {
			return fmt.Errorf("wal replay line %d: %w", lineNumber, err)
		}

		// Verify checksum (skip if 0 = old format without checksum)
		if record.Checksum != 0 {
			savedCRC := record.Checksum
			record.Checksum = 0
			cleanJSON, err := json.Marshal(record)
			if err != nil {
				return fmt.Errorf("wal replay line %d: marshal for checksum: %w", lineNumber, err)
			}
			computedCRC := crc32.ChecksumIEEE(cleanJSON)
			if savedCRC != computedCRC {
				return fmt.Errorf("wal replay line %d: checksum mismatch: saved=%d computed=%d",
					lineNumber, savedCRC, computedCRC)
			}
		}

		if err := handler(&record); err != nil {
			return err
		}
	}
	if err := scanner.Err(); err != nil {
		return fmt.Errorf("wal scan: %w", err)
	}
	return nil
}

// Truncate truncates the WAL file to zero length.
// Safe to call after a successful snapshot.
func (w *WAL) Truncate() error {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.file == nil {
		return nil
	}

	if err := w.file.Close(); err != nil {
		return fmt.Errorf("wal truncate close: %w", err)
	}

	file, err := os.OpenFile(w.path, os.O_CREATE|os.O_RDWR|os.O_TRUNC, 0o644)
	if err != nil {
		return fmt.Errorf("wal truncate reopen: %w", err)
	}
	w.file = file
	return nil
}

func (w *WAL) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.file == nil {
		return nil
	}
	return w.file.Close()
}


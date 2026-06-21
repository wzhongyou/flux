package flux

import (
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// ============================================================
// Configuration
// ============================================================

// Config represents the full Flux server configuration.
// It can be loaded from a YAML/JSON file, environment variables, or CLI flags.
type Config struct {
	Server   ServerConfig   `json:"server" yaml:"server"`
	Database DatabaseConfig `json:"database" yaml:"database"`
	Logging  LoggingConfig  `json:"logging" yaml:"logging"`
}

// ServerConfig holds HTTP server settings.
type ServerConfig struct {
	Addr          string   `json:"addr" yaml:"addr"`
	ReadTimeout   int      `json:"read_timeout" yaml:"read_timeout"`
	WriteTimeout  int      `json:"write_timeout" yaml:"write_timeout"`
	APIKeys       []string `json:"api_keys" yaml:"api_keys"`
	CORSOrigins   []string `json:"cors_origins" yaml:"cors_origins"`
	MaxConcurrent int      `json:"max_concurrent" yaml:"max_concurrent"`
	TLSKeyFile    string   `json:"tls_key_file" yaml:"tls_key_file"`
	TLSCertFile   string   `json:"tls_cert_file" yaml:"tls_cert_file"`
}

// TLSEnabled returns true if both cert and key files are configured.
func (s *ServerConfig) TLSEnabled() bool {
	return s.TLSCertFile != "" && s.TLSKeyFile != ""
}

// DatabaseConfig holds database engine settings.
type DatabaseConfig struct {
	WALPath          string `json:"wal_path" yaml:"wal_path"`
	SnapshotPath     string `json:"snapshot_path" yaml:"snapshot_path"`
	SnapshotInterval int    `json:"snapshot_interval" yaml:"snapshot_interval"` // seconds, 0 = disabled
}

// LoggingConfig holds logging settings.
type LoggingConfig struct {
	Level  string `json:"level" yaml:"level"`
	Pretty bool   `json:"pretty" yaml:"pretty"`
}

// DefaultConfig returns a Config with sensible defaults.
func DefaultConfig() *Config {
	return &Config{
		Server: ServerConfig{
			Addr:          ":8080",
			ReadTimeout:   30,
			WriteTimeout:  30,
			APIKeys:       nil,
			CORSOrigins:   nil,
			MaxConcurrent: 100,
		},
		Database: DatabaseConfig{
			WALPath:          "flux.wal",
			SnapshotPath:     "",
			SnapshotInterval: 0,
		},
		Logging: LoggingConfig{
			Level:  "info",
			Pretty: false,
		},
	}
}

// LoadConfig loads a configuration from the given file path.
// Supports JSON and JSON file formats.
// Environment variables and CLI flags take precedence.
func LoadConfig(path string) (*Config, error) {
	cfg := DefaultConfig()

	if path != "" {
		data, err := os.ReadFile(path)
		if err != nil {
			return nil, fmt.Errorf("read config: %w", err)
		}
		// Try JSON
		if err := json.Unmarshal(data, cfg); err != nil {
			return nil, fmt.Errorf("parse config: %w", err)
		}
	}

	// Environment variable overrides
	applyEnvOverrides(cfg)

	return cfg, nil
}

// applyEnvOverrides applies FLUX_* environment variables.
// Maps: FLUX_ADDR, FLUX_WAL_PATH, FLUX_API_KEYS, etc.
func applyEnvOverrides(cfg *Config) {
	if v := os.Getenv("FLUX_ADDR"); v != "" {
		cfg.Server.Addr = v
	}
	if v := os.Getenv("FLUX_WAL_PATH"); v != "" {
		cfg.Database.WALPath = v
	}
	if v := os.Getenv("FLUX_SNAPSHOT_PATH"); v != "" {
		cfg.Database.SnapshotPath = v
	}
	if v := os.Getenv("FLUX_SNAPSHOT_INTERVAL"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			cfg.Database.SnapshotInterval = n
		}
	}
	if v := os.Getenv("FLUX_API_KEYS"); v != "" {
		cfg.Server.APIKeys = strings.Split(v, ",")
	}
	if v := os.Getenv("FLUX_CORS_ORIGINS"); v != "" {
		cfg.Server.CORSOrigins = strings.Split(v, ",")
	}
	if v := os.Getenv("FLUX_MAX_CONCURRENT"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			cfg.Server.MaxConcurrent = n
		}
	}
	if v := os.Getenv("FLUX_LOG_LEVEL"); v != "" {
		cfg.Logging.Level = v
	}
	if v := os.Getenv("FLUX_LOG_PRETTY"); v != "" {
		cfg.Logging.Pretty = v == "true" || v == "1"
	}
	if v := os.Getenv("FLUX_READ_TIMEOUT"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			cfg.Server.ReadTimeout = n
		}
	}
	if v := os.Getenv("FLUX_WRITE_TIMEOUT"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			cfg.Server.WriteTimeout = n
		}
	}
	if v := os.Getenv("FLUX_TLS_CERT_FILE"); v != "" {
		cfg.Server.TLSCertFile = v
	}
	if v := os.Getenv("FLUX_TLS_KEY_FILE"); v != "" {
		cfg.Server.TLSKeyFile = v
	}
}

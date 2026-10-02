package main

import (
	"context"
	cryptorand "crypto/rand"
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"log"
	"math/rand/v2"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/redis/rueidis"
)

type config struct {
	address           string
	concurrency       int
	duration          time.Duration
	keyspace          int64
	valueSize         int
	setPercent        int64
	workload          string
	transactionSize   int
	connectionMode    string
	clientPerWorker   bool
	pipelineMultiplex int
	backendStatsAddr  string
	clusterHashTags   bool
	keyHashTag        string
	username          string
	password          string
	tls               bool
	cluster           bool
	tlsServerName     string
}

type counters struct {
	gets         atomic.Uint64
	sets         atomic.Uint64
	transactions atomic.Uint64
	misses       atomic.Uint64
	errors       atomic.Uint64
}

type connectionCounters struct {
	created atomic.Uint64
	closed  atomic.Uint64
}

type countedConnection struct {
	net.Conn
	closed atomic.Bool
	totals *connectionCounters
}

type redisConnectionStats struct {
	totalReceived uint64
	connected     uint64
}

const (
	workloadGetSet = "getset"
	workloadMulti  = "multi"

	connectionModePooled         = "pooled"
	connectionModePerTransaction = "per-transaction"
)

func main() {
	cfg := parseFlags()
	if err := run(cfg); err != nil {
		log.Fatal(err)
	}
}

func parseFlags() config {
	var cfg config
	flag.StringVar(&cfg.address, "addr", "127.0.0.1:7000", "Redis or muxproxy address")
	flag.IntVar(&cfg.concurrency, "concurrency", 50, "number of concurrent workers")
	flag.DurationVar(&cfg.duration, "duration", 10*time.Second, "benchmark duration")
	flag.Int64Var(&cfg.keyspace, "keyspace", 10000, "number of keys to use")
	flag.IntVar(&cfg.valueSize, "value-size", 128, "value size in bytes")
	flag.Int64Var(&cfg.setPercent, "set-percent", 20, "percentage of operations that are SETs")
	flag.StringVar(&cfg.workload, "workload", workloadGetSet, "workload to run: getset or multi")
	flag.IntVar(
		&cfg.transactionSize,
		"transaction-size",
		2,
		"number of GET/SET commands queued in each MULTI transaction",
	)
	flag.StringVar(
		&cfg.connectionMode,
		"connection-mode",
		connectionModePooled,
		"connection lifecycle: pooled or per-transaction",
	)
	flag.BoolVar(
		&cfg.clientPerWorker,
		"client-per-worker",
		false,
		"create one non-multiplexed rueidis client per worker",
	)
	flag.IntVar(
		&cfg.pipelineMultiplex,
		"pipeline-multiplex",
		0,
		"rueidis pipeline connection exponent; connections per node are 2^value (0-8)",
	)
	flag.StringVar(
		&cfg.backendStatsAddr,
		"backend-stats-addr",
		"",
		"optional direct Redis address used to report backend connections created and closed",
	)
	flag.BoolVar(
		&cfg.clusterHashTags,
		"cluster-hash-tags",
		false,
		"use one Redis Cluster hash tag for all keys in each MULTI transaction",
	)
	flag.StringVar(
		&cfg.keyHashTag,
		"key-hash-tag",
		"",
		"optional Redis Cluster hash tag applied to every benchmark key",
	)
	flag.StringVar(&cfg.username, "username", "", "frontend ACL username")
	flag.BoolVar(&cfg.tls, "tls", false, "use TLS for the Redis connection")
	flag.BoolVar(&cfg.cluster, "cluster", false, "use the Redis Cluster client")
	flag.StringVar(
		&cfg.tlsServerName,
		"tls-server-name",
		"",
		"TLS certificate server name; empty uses the host from -addr",
	)
	var passwordFile string
	flag.StringVar(
		&passwordFile,
		"password-file",
		"",
		"file containing the frontend password",
	)
	flag.Parse()

	if cfg.concurrency <= 0 {
		log.Fatal("-concurrency must be greater than zero")
	}
	if cfg.duration <= 0 {
		log.Fatal("-duration must be greater than zero")
	}
	if cfg.keyspace <= 0 {
		log.Fatal("-keyspace must be greater than zero")
	}
	if cfg.valueSize < 0 {
		log.Fatal("-value-size cannot be negative")
	}
	if cfg.setPercent < 0 || cfg.setPercent > 100 {
		log.Fatal("-set-percent must be between 0 and 100")
	}
	if cfg.workload != workloadGetSet && cfg.workload != workloadMulti {
		log.Fatal("-workload must be getset or multi")
	}
	if cfg.transactionSize <= 0 {
		log.Fatal("-transaction-size must be greater than zero")
	}
	if cfg.connectionMode != connectionModePooled &&
		cfg.connectionMode != connectionModePerTransaction {
		log.Fatal("-connection-mode must be pooled or per-transaction")
	}
	if cfg.connectionMode == connectionModePerTransaction && cfg.workload != workloadMulti {
		log.Fatal("-connection-mode per-transaction requires -workload multi")
	}
	if cfg.clientPerWorker && cfg.connectionMode == connectionModePerTransaction {
		log.Fatal("-client-per-worker cannot be combined with -connection-mode per-transaction")
	}
	if cfg.pipelineMultiplex < 0 || cfg.pipelineMultiplex > rueidis.MaxPipelineMultiplex {
		log.Fatalf(
			"-pipeline-multiplex must be between 0 and %d",
			rueidis.MaxPipelineMultiplex,
		)
	}
	if cfg.pipelineMultiplex != 0 &&
		(cfg.clientPerWorker || cfg.connectionMode == connectionModePerTransaction) {
		log.Fatal(
			"-pipeline-multiplex cannot be combined with dedicated client connection modes",
		)
	}
	if cfg.clusterHashTags && cfg.keyHashTag != "" {
		log.Fatal("-cluster-hash-tags and -key-hash-tag cannot be combined")
	}
	if passwordFile != "" {
		value, err := os.ReadFile(passwordFile)
		if err != nil {
			log.Fatalf("read -password-file: %v", err)
		}
		cfg.password = strings.TrimRight(string(value), "\r\n")
		if cfg.password == "" {
			log.Fatal("-password-file must contain a non-empty password")
		}
	}
	if cfg.username != "" && cfg.password == "" {
		log.Fatal("-username requires -password-file")
	}
	if !cfg.tls && cfg.tlsServerName != "" {
		log.Fatal("-tls-server-name requires -tls")
	}

	return cfg
}

func run(cfg config) error {
	parent, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	valueBytes := make([]byte, cfg.valueSize)
	if _, err := cryptorand.Read(valueBytes); err != nil {
		return fmt.Errorf("generate benchmark value: %w", err)
	}
	value := string(valueBytes)

	var connections connectionCounters
	clientCount := 1
	if cfg.clientPerWorker {
		clientCount = cfg.concurrency
	}
	clients := make([]rueidis.Client, 0, clientCount)
	for range clientCount {
		client, err := newClient(cfg, &connections)
		if err != nil {
			closeClients(clients)
			return fmt.Errorf("create Redis client: %w", err)
		}
		clients = append(clients, client)
	}
	pingCtx, cancelPing := context.WithTimeout(parent, 5*time.Second)
	defer cancelPing()
	if err := clients[0].Do(pingCtx, clients[0].B().Ping().Build()).Error(); err != nil {
		closeClients(clients)
		return fmt.Errorf("connect to Redis at %s: %w", cfg.address, err)
	}

	var statsClient rueidis.Client
	var statsBefore redisConnectionStats
	var err error
	if cfg.backendStatsAddr != "" {
		statsClient, err = rueidis.NewClient(rueidis.ClientOption{
			InitAddress:       []string{cfg.backendStatsAddr},
			ForceSingleClient: true,
			AlwaysRESP2:       true,
			DisableCache:      true,
			ClientSetInfo:     rueidis.DisableClientSetInfo,
		})
		if err != nil {
			closeClients(clients)
			return fmt.Errorf(
				"create Redis stats client for %s: %w",
				cfg.backendStatsAddr,
				err,
			)
		}
		statsBefore, err = readRedisConnectionStats(pingCtx, statsClient)
		if err != nil {
			closeClients(clients)
			statsClient.Close()
			return fmt.Errorf(
				"read Redis connection stats from %s: %w",
				cfg.backendStatsAddr,
				err,
			)
		}
	}

	ctx, cancel := context.WithTimeout(parent, cfg.duration)
	defer cancel()

	var totals counters
	var workers sync.WaitGroup
	start := time.Now()

	workers.Add(cfg.concurrency)
	for workerID := range cfg.concurrency {
		go func() {
			defer workers.Done()
			client := clients[0]
			if cfg.clientPerWorker {
				client = clients[workerID]
			}
			runWorker(
				ctx,
				client,
				cfg,
				workerID,
				value,
				&totals,
				&connections,
			)
		}()
	}
	workers.Wait()

	elapsed := time.Since(start)
	closeClients(clients)

	var backendCreated uint64
	var backendClosed uint64
	if statsClient != nil {
		time.Sleep(500 * time.Millisecond)
		statsAfter, err := readRedisConnectionStats(parent, statsClient)
		if err != nil {
			statsClient.Close()
			return fmt.Errorf(
				"read final Redis connection stats from %s: %w",
				cfg.backendStatsAddr,
				err,
			)
		}
		backendCreated = statsAfter.totalReceived - statsBefore.totalReceived
		backendClosed = backendCreated + statsBefore.connected - statsAfter.connected
		statsClient.Close()
	}

	gets := totals.gets.Load()
	sets := totals.sets.Load()
	transactions := totals.transactions.Load()
	misses := totals.misses.Load()
	failures := totals.errors.Load()
	operations := gets + sets

	if cfg.workload == workloadMulti {
		commands := operations + 2*transactions
		fmt.Printf(
			"address=%s workload=%s connection_mode=%s client_per_worker=%t concurrency=%d transaction_size=%d elapsed=%s transactions=%d transactions/sec=%.0f operations=%d ops/sec=%.0f commands=%d commands/sec=%.0f gets=%d sets=%d misses=%d errors=%d client_connections_created=%d client_connections_closed=%d",
			cfg.address,
			cfg.workload,
			cfg.connectionMode,
			cfg.clientPerWorker,
			cfg.concurrency,
			cfg.transactionSize,
			elapsed.Round(time.Millisecond),
			transactions,
			float64(transactions)/elapsed.Seconds(),
			operations,
			float64(operations)/elapsed.Seconds(),
			commands,
			float64(commands)/elapsed.Seconds(),
			gets,
			sets,
			misses,
			failures,
			connections.created.Load(),
			connections.closed.Load(),
		)
		if statsClient != nil {
			fmt.Printf(
				" backend_address=%s backend_connections_created=%d backend_connections_closed=%d",
				cfg.backendStatsAddr,
				backendCreated,
				backendClosed,
			)
		}
		fmt.Println()
	} else {
		fmt.Printf(
			"address=%s workload=%s connection_mode=%s client_per_worker=%t concurrency=%d elapsed=%s operations=%d ops/sec=%.0f gets=%d sets=%d misses=%d errors=%d client_connections_created=%d client_connections_closed=%d\n",
			cfg.address,
			cfg.workload,
			cfg.connectionMode,
			cfg.clientPerWorker,
			cfg.concurrency,
			elapsed.Round(time.Millisecond),
			operations,
			float64(operations)/elapsed.Seconds(),
			gets,
			sets,
			misses,
			failures,
			connections.created.Load(),
			connections.closed.Load(),
		)
	}

	if failures > 0 {
		return fmt.Errorf("%d Redis operations failed", failures)
	}
	return nil
}

func closeClients(clients []rueidis.Client) {
	for _, client := range clients {
		client.Close()
	}
}

func runWorker(
	ctx context.Context,
	client rueidis.Client,
	cfg config,
	workerID int,
	value string,
	totals *counters,
	connections *connectionCounters,
) {
	random := rand.New(rand.NewPCG(uint64(workerID), uint64(workerID+1)))
	var operation uint64
	for {
		if err := ctx.Err(); err != nil {
			return
		}

		if cfg.workload == workloadMulti {
			runTransaction(
				ctx,
				client,
				cfg,
				workerID,
				value,
				totals,
				connections,
				random,
				&operation,
			)
			continue
		}

		key := nextKey(cfg, workerID, &operation)
		if random.Int64N(100) < cfg.setPercent {
			err := client.Do(
				ctx,
				client.B().Set().Key(key).Value(value).Build(),
			).Error()
			if err != nil {
				if !isContextDone(err) {
					totals.errors.Add(1)
				}
				continue
			}
			totals.sets.Add(1)
			continue
		}

		err := client.Do(ctx, client.B().Get().Key(key).Build()).Error()
		switch {
		case err == nil:
			totals.gets.Add(1)
		case rueidis.IsRedisNil(err):
			totals.gets.Add(1)
			totals.misses.Add(1)
		case !isContextDone(err):
			totals.errors.Add(1)
		}
	}
}

func runTransaction(
	ctx context.Context,
	client rueidis.Client,
	cfg config,
	workerID int,
	value string,
	totals *counters,
	connections *connectionCounters,
	random *rand.Rand,
	operation *uint64,
) {
	transactionClient := client
	if cfg.connectionMode == connectionModePerTransaction {
		var err error
		transactionClient, err = newClient(cfg, connections)
		if err != nil {
			totals.errors.Add(1)
			return
		}
		defer transactionClient.Close()
	}

	var gets uint64
	var sets uint64
	var getIndexes []int
	transactionTag := ""
	if cfg.clusterHashTags {
		tag := (uint64(workerID) + *operation*uint64(cfg.concurrency)) %
			uint64(cfg.keyspace)
		transactionTag = fmt.Sprintf("{benchclient:%d}:", tag)
	}
	var transactionResults []rueidis.RedisResult
	err := transactionClient.Dedicated(func(dedicated rueidis.DedicatedClient) error {
		commands := make([]rueidis.Completed, 0, cfg.transactionSize+2)
		keys := make([]string, 0, cfg.transactionSize)
		for range cfg.transactionSize {
			key := transactionTag + nextKey(cfg, workerID, operation)
			keys = append(keys, key)
			if random.Int64N(100) < cfg.setPercent {
				commands = append(
					commands,
					dedicated.B().Set().Key(key).Value(value).Build(),
				)
				sets++
			} else {
				getIndexes = append(getIndexes, len(commands))
				commands = append(commands, dedicated.B().Get().Key(key).Build())
				gets++
			}
		}
		slotKey := keys[0]
		multi := dedicated.B().Multi().Build().SetSlot(slotKey)
		exec := dedicated.B().Exec().Build().SetSlot(slotKey)
		pipeline := make([]rueidis.Completed, 0, len(commands)+2)
		pipeline = append(pipeline, multi)
		pipeline = append(pipeline, commands...)
		pipeline = append(pipeline, exec)
		transactionResults = dedicated.DoMulti(ctx, pipeline...)
		return nil
	})
	if err != nil {
		if !isContextDone(err) {
			totals.errors.Add(1)
		}
		return
	}
	if len(transactionResults) != cfg.transactionSize+2 {
		totals.errors.Add(1)
		return
	}
	for _, result := range transactionResults[:len(transactionResults)-1] {
		if err := result.Error(); err != nil {
			if !isContextDone(err) {
				totals.errors.Add(1)
			}
			return
		}
	}
	execResults, err := transactionResults[len(transactionResults)-1].ToArray()
	if err != nil || len(execResults) != cfg.transactionSize {
		if err == nil || !isContextDone(err) {
			totals.errors.Add(1)
		}
		return
	}

	var misses uint64
	for _, index := range getIndexes {
		switch err := execResults[index].Error(); {
		case err == nil:
		case rueidis.IsRedisNil(err):
			misses++
		case isContextDone(err):
			return
		default:
			totals.errors.Add(1)
			return
		}
	}

	totals.gets.Add(gets)
	totals.sets.Add(sets)
	totals.misses.Add(misses)
	totals.transactions.Add(1)
}

func nextKey(cfg config, workerID int, operation *uint64) string {
	keyNumber := (uint64(workerID) + *operation*uint64(cfg.concurrency)) % uint64(cfg.keyspace)
	*operation++
	if cfg.keyHashTag != "" {
		return fmt.Sprintf("{%s}:benchclient:%d", cfg.keyHashTag, keyNumber)
	}
	return fmt.Sprintf("benchclient:%d", keyNumber)
}

func newClient(cfg config, totals *connectionCounters) (rueidis.Client, error) {
	netDialer := &net.Dialer{}
	var tlsConfig *tls.Config
	if cfg.tls {
		serverName := cfg.tlsServerName
		if serverName == "" {
			var err error
			serverName, _, err = net.SplitHostPort(cfg.address)
			if err != nil {
				log.Fatalf("parse -addr for TLS server name: %v", err)
			}
		}
		tlsConfig = &tls.Config{
			MinVersion: tls.VersionTLS12,
			ServerName: serverName,
		}
	}
	dial := func(
		ctx context.Context,
		address string,
		dialer *net.Dialer,
		tlsConfig *tls.Config,
	) (net.Conn, error) {
		var connection net.Conn
		var err error
		if tlsConfig == nil {
			connection, err = dialer.DialContext(ctx, "tcp", address)
		} else {
			tlsDialer := &tls.Dialer{
				NetDialer: dialer,
				Config:    tlsConfig,
			}
			connection, err = tlsDialer.DialContext(ctx, "tcp", address)
		}
		if err != nil {
			return nil, err
		}
		totals.created.Add(1)
		return &countedConnection{
			Conn:   connection,
			totals: totals,
		}, nil
	}
	pipelineMultiplex := cfg.pipelineMultiplex
	if cfg.clientPerWorker || cfg.connectionMode == connectionModePerTransaction {
		pipelineMultiplex = -1
	}
	return rueidis.NewClient(rueidis.ClientOption{
		InitAddress:       []string{cfg.address},
		Username:          cfg.username,
		Password:          cfg.password,
		TLSConfig:         tlsConfig,
		Dialer:            *netDialer,
		DialCtxFn:         dial,
		ForceSingleClient: !cfg.cluster,
		AlwaysRESP2:       true,
		DisableCache:      true,
		PipelineMultiplex: pipelineMultiplex,
		ClientSetInfo:     rueidis.DisableClientSetInfo,
	})
}

func (connection *countedConnection) Close() error {
	if connection.closed.CompareAndSwap(false, true) {
		connection.totals.closed.Add(1)
	}
	return connection.Conn.Close()
}

func readRedisConnectionStats(
	ctx context.Context,
	client rueidis.Client,
) (redisConnectionStats, error) {
	info, err := client.Do(ctx, client.B().Info().Build()).ToString()
	if err != nil {
		return redisConnectionStats{}, err
	}

	var stats redisConnectionStats
	var foundTotal bool
	var foundConnected bool
	for _, line := range strings.Split(info, "\r\n") {
		name, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		switch name {
		case "total_connections_received":
			stats.totalReceived, err = strconv.ParseUint(value, 10, 64)
			foundTotal = err == nil
		case "connected_clients":
			stats.connected, err = strconv.ParseUint(value, 10, 64)
			foundConnected = err == nil
		}
		if err != nil {
			return redisConnectionStats{}, fmt.Errorf("parse %s: %w", name, err)
		}
	}
	if !foundTotal || !foundConnected {
		return redisConnectionStats{}, errors.New(
			"Redis INFO did not contain connection counters",
		)
	}
	return stats, nil
}

func isContextDone(err error) bool {
	return errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded)
}

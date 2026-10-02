docker run --rm \
  --network host \
  --cpuset-cpus 0,2,4,6 \
  --ulimit nofile=1048576:1048576 \
  --mount type=bind,src="$PWD/amr-password",dst=/run/secrets/amr-password,readonly \
  ghcr.io/owner/seaproxy:0.1.0 \
  --smp 4 \
  --cpuset 0,2,4,6 \
  --memory 2G \
  --redis-address example.redis.azure.net \
  --redis-port 10000 \
  --redis-mode cluster \
  --redis-tls \
  --redis-username default \
  --redis-password-file /run/secrets/amr-password
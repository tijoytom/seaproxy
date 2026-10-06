docker run --rm \
  --network host \
  --cpuset-cpus 0,2,4,6 \
  --ulimit nofile=1048576:1048576 \
  --mount type=bind,src="$PWD/seaproxy.toml",dst=/etc/seaproxy/config.toml,readonly \
  --mount type=bind,src="$PWD/amr-password",dst=/run/secrets/amr-password,readonly \
  ghcr.io/owner/seaproxy:0.1.0 \
  --config /etc/seaproxy/config.toml \
  --smp 4 \
  --cpuset 0,2,4,6 \
  --memory 2G
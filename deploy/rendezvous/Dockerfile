FROM debian:bookworm-slim
COPY nyx-rendezvous /usr/local/bin/nyx-rendezvous
EXPOSE 3478/udp
ENTRYPOINT ["/usr/local/bin/nyx-rendezvous", "--bind=0.0.0.0:3478", "--rate-limit=240"]

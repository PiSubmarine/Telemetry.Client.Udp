# Telemetry.Client.Udp

`PiSubmarine.Telemetry.Client.Udp` provides a remote raw telemetry cache and a
per-channel raw source adapter.

## Responsibility

This module owns:

- automatic acquisition and renewal of the telemetry lease
- sending UDP subscription packets containing the current `LeaseId`
- receiving UDP telemetry datagrams
- decoding UDP telemetry datagrams into `ChannelId -> raw payload` entries
- exposing the latest received payloads through `Telemetry.Api::IRawCache`
- exposing individual channels through `Telemetry.Client.Udp::Source`

It does not own:

- lease issuance policy
- UDP socket implementation
- domain-specific telemetry deserialization

## Runtime model

The client is tick-driven.

- On first use, it acquires the `telemetry-main` lease.
- After acquiring or renewing a lease, it sends a UDP subscription packet whose
  payload is just the raw `LeaseId`.
- It renews the lease halfway through the reported lease duration.
- It drains received UDP datagrams each tick and replaces the cached raw
  channel map whenever a datagram can be decoded successfully.

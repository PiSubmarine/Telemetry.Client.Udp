# Telemetry.Client.Udp

`PiSubmarine.Telemetry.Client.Udp` provides a remote telemetry source that looks
like a local `Telemetry.Api::ISource` to its consumers.

## Responsibility

This module owns:

- automatic acquisition and renewal of the telemetry lease
- sending UDP subscription packets containing the current `LeaseId`
- receiving UDP telemetry datagrams
- deserializing telemetry payloads into `Telemetry.Api::Snapshot`
- exposing the latest received snapshot through `Telemetry.Api::ISource`

It does not own:

- lease issuance policy
- UDP socket implementation
- telemetry payload format

## Runtime model

The client is tick-driven.

- On first use, it acquires the `telemetry-main` lease.
- After acquiring or renewing a lease, it sends a UDP subscription packet whose
  payload is just the raw `LeaseId`.
- It renews the lease halfway through the reported lease duration.
- It drains received UDP datagrams each tick and updates the cached snapshot
  whenever deserialization succeeds.

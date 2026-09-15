# Architecture

## Overview

NeedLove uses a thin-client architecture. The server is the source of truth;
clients never decide whether a match exists or directly exchange messages.

## Server event loop

The listening socket and every accepted client socket are nonblocking. During
each loop iteration, `select()` identifies sockets ready to accept, read, or
write. Incoming bytes accumulate in a per-connection buffer until a complete
newline-delimited command is available.

Responses are appended to a per-connection output queue. This design handles
partial writes and prevents one slow receiver from pausing every client.

## Data model

- `User` stores profile, connection, swipe, match, block, and unread state.
- `Connection` stores socket-specific input and output buffers.
- `Conversation` owns a bounded circular buffer of chat messages.

User relationships are indexed by fixed-size arrays. Conversations normalize
the pair `(a, b)` so both directions resolve to one shared history.

## Persistence

The server loads `state.txt` on startup and writes it after state-changing
commands and during graceful shutdown. Runtime-only fields such as socket file
descriptors and open-chat status are not restored.

## Reliability choices

- Bounded buffers and field validation limit malformed input.
- Queued output handles partial sends and temporary backpressure.
- `SIGPIPE` is ignored so a disconnected peer cannot terminate the process.
- `SIGINT` sets a flag, allowing state to be saved before shutdown.
- Chat history is capped and uses a circular buffer to bound memory use.

## Current limits

The implementation supports at most 100 users and stores at most 100 messages
per conversation. Persistence is human-readable but is not transactional.
Production evolution would add authenticated accounts, Transport Layer
Security, a database, structured protocol encoding, automated concurrency
tests, and separate modules for networking, storage, and domain logic.


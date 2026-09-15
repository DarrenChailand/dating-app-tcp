# Wire protocol

NeedLove uses UTF-8-compatible, newline-delimited text over TCP. Fields are
separated by `|`; list items inside a field may use commas and colons. The
server is authoritative and may send asynchronous notifications between normal
responses.

## Client commands

| Command | Purpose |
| --- | --- |
| `LOGIN|username` | Create or reconnect to an offline account. |
| `PROFILE_SET|age|gender|preference|bio` | Create or replace a profile. |
| `PROFILE_GET` | Fetch the signed-in profile. |
| `REQUEST_CANDIDATES|count` | Request compatible, unseen profiles. |
| `SWIPE_BATCH|alice:L,bob:D` | Submit like or dislike decisions. |
| `MATCH_LIST` | Fetch matches, presence, and unread counts. |
| `CHAT_OPEN|username` | Open a match and retrieve chat history. |
| `CHAT_SEND|username|message` | Send and persist a message. |
| `CHAT_CLOSE` | Leave the current chat. |
| `UNMATCH|username` | Remove and permanently block a match. |
| `QUIT` | Request a graceful disconnect. |

## Server events

| Event | Meaning |
| --- | --- |
| `WELCOME|message` | Initial greeting after connection. |
| `LOGIN_OK` | Login accepted. |
| `PROFILE|...` | Profile payload. |
| `CANDIDATE|...` | One profile inside candidate begin/end markers. |
| `MATCH|username|status|unread` | One match inside list markers. |
| `MATCH_NEW|username` | A mutual like created a match. |
| `NOTIFY_UNREAD|count` | Total unread count changed. |
| `CHAT_MSG|sender|message` | One historical chat message. |
| `CHAT_APPEND|sender|message` | A live incoming chat message. |
| `CHAT_SELF|message` | Confirmation and echo of a sent message. |
| `UNMATCHED_BY|username` | Another user removed the match. |
| `ERROR|message` | Command rejected. |
| `BYE` | Server accepted graceful disconnect. |

## Framing and sanitization

Every frame ends with `\n`. TCP may split or combine frames, so both programs
buffer bytes and process only complete lines. Profile fields replace protocol
delimiters with `/`; chat messages replace `|` with `/`. Oversized frames are
rejected.


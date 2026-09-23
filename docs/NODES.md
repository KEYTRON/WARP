# WARP repositories and nodes

Language: English | [Русский](NODES.ru.md)

**Status: design, not implemented.** Command and field names are provisional.

## Why

- **Repository mode** — anyone, a person or a company, can run their own package repository with one or two commands.
- **Node mode** — nodes take over serving all of a repository's packages or a share of them. This offloads the repository host and gives horizontal scaling.
- **Adding repositories** already exists in the client (`warp repo add … --pubkey`) and stays the main way to trust a repository.

## The core principle

The package world is closed: a package exists only when the repository has signed its name, version and sha256. Nodes are transport. They cannot add a package, change it or slip in another version: the client checks every downloaded byte against the signed index.

Signing needs only the sha256, not the file itself. So a package may live only on nodes (when the host runs out of disk or bandwidth) and still be signed by the repository.

## Key roles

Today one key does everything. So that stealing one key does not let anyone ship malware, the roles are split:

| Key | Where it lives | What it signs | If it is stolen |
|---|---|---|---|
| Root | offline | `root.json`: which role keys are valid and until when | everything — which is why it stays offline and is used rarely |
| Index key | not on the serving host | `index.json`: packages, sha256, deltas | malware can be signed — replaced with the root key |
| Online key | on the server | `nodes.json` and `timestamp.json`, short-lived | only the node list and freshness, within their lifetime; packages cannot be signed |

The key pinned by the client (today's `--pubkey`) becomes the root key. Role keys are rotated with a new `root.json` signed by the root key. The root key itself is rotated with a new `root.json` signed by both the old and the new root key.

## Repository files

| File | Signed by | Lifetime | Contents |
|---|---|---|---|
| `root.json` | root key | long (e.g. a year) | role keys and their lifetimes |
| `index.json` | index key | medium | packages, sha256, sizes, deltas, locations; an increasing `version` |
| `nodes.json` | online key | short (e.g. a day) | node certificates |
| `timestamp.json` | online key | very short (e.g. an hour) | version and sha256 of the current `index.json` and `nodes.json` |

## Rollback protection — the first step

Today the client reads the index `timestamp` but never compares it with what it has already seen. Anyone serving the index can hand out an old but genuinely signed index and keep the client on versions with known vulnerabilities. So before nodes:

- every signed file carries an increasing `version` and an `expires` date;
- the client remembers the last version it saw and rejects a file with a lower version or an expired one, with a clear error.

Node mode is not enabled without this.

## Node certificate

An entry in `nodes.json`:

```json
{
  "node_id": "fra-1",
  "pubkey": "<the node's Ed25519 key>",
  "urls": ["https://mirror.example/warp"],
  "scope": {"packages": ["rust", "go"]},
  "issued": "2026-09-23T12:00:00Z",
  "expires": "2026-09-24T12:00:00Z"
}
```

`scope` is all packages or a share of them: a list of names or a hash range for even sharding.

## Joining — both sides agree

1. **The node operator** creates a node key and sends the repository a request: the key, addresses and the share it is willing to store. The request is signed with the node key, proving the node owns it.
   `warp node init` → `warp node request <repository-url>`
2. **The repository owner** sees the request and approves it with a scope and a lifetime.
   `warp repo nodes pending` → `warp repo nodes approve <id> --scope … --days N`
3. **Sync.** The node downloads the packages in its scope and checks each against the sha256 in the signed index. On a mismatch it does not serve the file.
4. **Readiness check.** The repository asks the node for random chunks of packages in its scope. Only after it answers does the node enter `nodes.json`.
5. **Renewal.** The online key periodically rebuilds `nodes.json` with a new lifetime, but only for nodes that pass the availability check. A node that disappears drops out at the next renewal on its own.
6. **Revocation.** The node is removed from `nodes.json`; clients learn about it with the next `timestamp.json`.

By default only approved nodes enter a repository. Open registration may become an option later: it is easier to flood with dead addresses. Today's `warp seed` / `warp volunteer` become a special case of a node.

## How the client downloads

1. `warp update`: `timestamp.json` → `index.json` and `nodes.json`. Signatures, versions and lifetimes are checked.
2. For a package, the candidates are the repository itself and every node with a valid certificate whose scope covers the package.
3. The client tries them in order (by latency or at random) and checks the sha256. On a mismatch, an error or a slow response the node is marked bad and the next one is used.

## Packages from nodes — delegation, not their own signature

A node **does not sign** packages on behalf of the repository: otherwise a compromised node ships malware under someone else's trust. If a node needs to publish its own:

- **a separate repository** with its own key — the user adds it explicitly;
- **namespace delegation** — the root key allows the node key to sign only packages with a prefix, e.g. `acme/*`. The client shows the source, and the node can never override base packages (`warp`, `openssl` and so on).

## Risks

- **Privacy:** a node sees the client's IP and what it downloads. Companies need to be told this explicitly.
- **Unavailable or slow nodes:** the client silently switches to other nodes or to the repository.
- **A stolen online key:** can spoil the node list or delay updates within the lifetime of `nodes.json` / `timestamp.json`, but cannot sign a package.

## Stages

1. Rollback protection.
2. Repository mode: `warp repo init` and `warp publish` instead of an external script, split keys.
3. Node mode: certificates, agreement, sync, checks, renewal, client downloads from nodes.
4. Namespace delegation (if needed).

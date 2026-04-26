<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Two authorities

A deployed SynQt system uses two separate kinds of certificate. Mix them up and the system
either does not start or is less private than it looks.

- **The public certificate** is for the browser. An authority the world already trusts
  issues it, for a name in DNS. Only the web edge has one.
- **The mesh certificates** are for your entities. An authority you created issues them,
  for names that mean nothing outside your project. Each service entity has one, and no
  browser ever sees them.

They answer different questions. The public certificate answers "is this really
gavel.example.com?" A mesh certificate answers "is the caller of `recordWinner` really the
web edge?" No public authority can answer the second, so you become an authority.

## Step 1: Create the authority

On your own machine, in the project directory:

```cli
synqt mesh init
```

This writes `synqt/mesh/ca.key` and `synqt/mesh/ca.crt`, restricts the key to your user,
and adds the ignore rules that keep the key out of git. Read the output: if it says the
permissions could not be set on this platform, fix that before you go on.

Now issue one certificate per service entity:

```cli
synqt mesh cert --all
synqt mesh status
```

```text
Certificates in synqt/mesh:
  books: valid until 2027-09-05 (397 days)
  ca: valid until 2028-10-07 (795 days)
  edge: valid until 2027-09-05 (397 days)
```

Each entity certificate carries the entity name as its subject, and that is how
`Caller.entity` works. When the database checks `Caller.entity === "edge"`, it reads the
name from a certificate whose key the other end proved it holds, issued by an authority
both ends verify against.

The client gets no certificate. A browser authenticates with a user session, never with a
mesh identity, and the two never substitute for each other. If you want to issue one to a
client, you want a scope instead.

## Step 2: Decide where the key lives, once

This is the one decision on this page; the rest are commands.

The CA private key never goes on a host that runs an entity, and never into CI. Anyone
holding `ca.key` can create a certificate that says `edge`, and every entity in your system
will believe it.

Pick one:

- **Solo project:** the key stays on your machine, with an encrypted backup outside the
  repository. You issue certificates before a deploy.
- **Team:** the key lives in a secret store (a password manager with file support, a cloud
  KMS, a hardware token), and issuing is a step someone runs and logs, so it can be
  audited later.

Each host gets only what it needs:

| File | Edge host | Database host | Your machine | CI |
|------|-----------|---------------|--------------|-----|
| `synqt/mesh/ca.crt` | yes | yes | yes | no |
| `synqt/mesh/ca.key` | **no** | **no** | yes | **no** |
| `synqt/mesh/edge.crt` and `.key` | yes | no | yes | no |
| `synqt/mesh/books.crt` and `.key` | no | yes | yes | no |

A database host has no reason to hold the edge's key. Giving it one for convenience means
a compromised database compromises the edge too.

> [!WARNING]
> `synqt mesh init` and the scaffolder make git ignore `synqt/mesh/*.key`. Check that it
> is still ignored before your first push. Deleting the file does not remove a private key
> from git history; the only fix is a new authority and new certificates for everything
> under it.

## Step 3: The certificate the browser wants

Get a certificate for your domain the usual way: an ACME client such as certbot or your
host's built-in one, a certificate your organization issues, anything that produces a full
chain and a private key. SynQt leaves renewal to those tools, because good ones already
exist.

The files must be PEM, with an unencrypted RSA or elliptic curve key, since nobody is there
to type a passphrase. An edge given a pair it cannot read says so and does not start. The
same goes for a key its Qt cannot present: an elliptic curve key needs the OpenSSL TLS
backend, so if you run the edge on macOS, where Qt uses Secure Transport, request an RSA
key (`certbot --key-type rsa`).

Put the two files where the edge's `tls:` block in `synqt.yaml` says they are:

```text
gavel/
  certs/edge/fullchain.pem
  certs/edge/privkey.pem
```

The paths are relative to the project root, like everything an entity reads.

Alternatively, let something in front of the edge terminate TLS:

```yaml
# synqt.production.yaml, instead of the tls block
entities:
  - name: edge
    public:
      tls_terminated_upstream: true
      origin: https://gavel.example.com
```

Then the edge listens in plaintext on loopback, and your reverse proxy holds the public
certificate. Both setups work, and a release build refuses to guess which you mean.

> [!IMPORTANT]
> With a proxy in front, do not let it rewrite response headers. The edge computes the
> Content-Security-Policy from your topology, including the sync endpoint's exact `wss://`
> origin, and sends the cross origin isolation headers when the client is multi threaded.
> A proxy that replaces them breaks the client, and the failure shows in the browser, not
> in your logs. See [Content-Security-Policy](csp.md).

## Step 4: The secrets, which are not certificates

The auction signs people in through GitHub, so the edge holds an OAuth client secret; the
database has no secret yet. Secrets never go in `synqt.yaml`.

Declare a secret as a reference:

```yaml
identity:
  providers:
    - name: github
      client_id: Iv1.0123456789abcdef
      client_secret: env:GITHUB_CLIENT_SECRET
```

and resolved at start from the entity's own env file:

```text
# web/edge/.env on the edge host, readable only by the user the edge runs as
GITHUB_CLIENT_SECRET=the-real-value
```

Two rules are enforced, not just recommended, and they rule out whole classes of mistake:

- **A provider password or connection URI, and an identity provider's `client_secret`,
  must be `env:` references.** You cannot paste a literal secret into the topology, so it
  cannot slip into the repository.
- **An `env:` reference reachable from a client target is rejected.** A secret cannot
  reach the browser by being named in the wrong section, because that section is
  refused.

Each entity directory has an `.env.example` listing the names that entity expects. Copy it
to `.env` on the host, fill it in, and restrict its permissions (`chmod 600`).

## Step 5: Watch it refuse

Certificates are checked at start, not by `synqt check`, because the CA must not exist on
a build machine. See it now rather than at three in the morning:
move the database certificate aside and try to start.

```cli
mv synqt/mesh/books.crt /tmp/
synqt serve --profile production
```

```text
error: entity 'books' is on a mutual-TLS link with no certificate in synqt/mesh/;
run 'synqt mesh cert books' (synqt dev issues development certificates itself)
synqt: refusing to continue with an invalid configuration (run 'synqt check' for the
full report).
```

Put it back. The failure names the entity, the directory and the command, and it happens
before anything listens on a port. Plain `synqt check` reports the same problem as a
warning, because a development run has its own certificates; `synqt serve` makes it an
error when it starts a deployment.

## Try it, then think

> [!QUESTION]
> The edge and the database will share a host at first, to keep the first deploy simple.
> Mutual TLS on a loopback link seems pointless, since nothing untrusted can reach
> `127.0.0.1`. Can you turn it off, and should you?

<details class="solution" markdown>
<summary>Solution</summary>

You can: `transport: local` replaces the TLS socket with a Unix domain socket restricted
to the user the entities run as. It is faster and documented, and SynQt never picks it
for you.

It costs the meaning of `Caller.entity`. On a local link the OS reports which user
connected, not which entity, so any process of that user can claim to be the edge. The
consumer list, and any `Caller.entity === "edge"` check, stop authenticating anything and
become an assumption about who else is on the machine. That is why `synqt check` flags every local
link.

Mutual TLS on loopback costs one handshake per connection, once per link, not per call.
Keep it. When the database moves to its own host next week, its trust position stays the
same.

[The entity to entity links](security.md#the-entity-to-entity-links-the-mesh) has the full
comparison.

</details>

## Advice worth taking now

- **Put the expiry in a calendar.** Entity certificates last 398 days, the CA twice that.
  `synqt mesh status` warns 30 days ahead, but only if someone runs it. A reminder a month
  before the first expiry costs nothing, and prevents an outage that looks like a network
  fault in the logs.
- **Rotate one entity at a time.** `synqt mesh rotate books` issues a new leaf from the same
  authority. Copy the new pair to that host and restart that entity. Its peers verify
  against the unchanged CA certificate, so nothing else needs to change.
- **Schedule an authority rotation.** Every entity trusts exactly one CA certificate, so
  an authority rotation has no overlap period. A new authority means new leaves everywhere and a
  coordinated restart; plan a maintenance window for it.
- **Never reuse the development CA.** `synqt dev` keeps a throwaway authority under
  `synqt/mesh/dev/` so development keeps mutual TLS with no setup. It is separate, and a
  release build refuses it.

Next: [Where the binaries go](tutorial-ship-hosts.md), and the layout that makes these
paths resolve.

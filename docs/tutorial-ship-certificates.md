<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Two authorities

A deployed SynQt system uses two completely separate kinds of certificate, and confusing
them is the most common way to end up with something that either does not start or is
not as private as it looks.

- **The public certificate.** For the browser. Issued by an authority the world already
  trusts, for a name in DNS. One of these, on the web edge, and nothing else in the
  system has one.
- **The mesh certificates.** For your entities. Issued by an authority you created, for
  names that mean nothing outside your project. One per service entity, and no browser
  will ever see one.

They answer different questions. The public certificate answers "is this really
gavel.example.com". The mesh certificates answer "is the thing calling `recordWinner`
really the web edge". No public authority can answer the second question, which is why
you are about to become an authority.

## Step 1: Create the authority

On your own machine, in the project directory:

```cli
synqt mesh init
```

That writes `synqt/mesh/ca.key` and `synqt/mesh/ca.crt`, restricts the key to your user,
and adds the ignore rules that keep the key out of git. Read the output: if it tells you
the permissions could not be set on this platform, believe it and fix it before going on.

Now issue one certificate per service entity:

```cli
synqt mesh cert --all
synqt mesh status
```

```text
ca                valid until 2029-09-05  (1128 days)
database.crt      valid until 2027-09-05  (398 days)
web.crt           valid until 2027-09-05  (398 days)
```

Each entity certificate carries the entity name as its subject. That is the whole
mechanism behind `Caller.entity`: when the database checks `Caller.entity === "web"`, it
is reading a name out of a certificate the other end proved it holds the key for, issued
by an authority both of them verify against.

There is no certificate for the client. A browser authenticates with a user session,
never with a mesh identity, and the two are never interchangeable. If you find yourself
wanting to issue one, what you actually want is a scope.

## Step 2: Decide where the key lives, once

This is the decision on this page. Everything else is a command.

**The CA private key never goes on a host that runs an entity, and never into CI.**
Anyone holding `ca.key` can mint a certificate that says `web` on it, and every entity in
your system will believe them. It is not a deployment input; it is the thing that makes
deployment inputs meaningful.

Practically, pick one:

- **Solo project:** the key stays on your machine, backed up somewhere encrypted that is
  not the repository. Issuing is something you do before a deploy.
- **A team:** the key lives in a secret store (a password manager with file support, a
  cloud KMS, a hardware token), and issuing is a deliberate step somebody runs, logs, and
  can be asked about later.

What each host gets is the small half:

| File | Edge host | Database host | Your machine | CI |
|------|-----------|---------------|--------------|-----|
| `synqt/mesh/ca.crt` | yes | yes | yes | no |
| `synqt/mesh/ca.key` | **no** | **no** | yes | **no** |
| `synqt/mesh/web.crt` and `.key` | yes | no | yes | no |
| `synqt/mesh/database.crt` and `.key` | no | yes | yes | no |

A database host has no reason to hold the edge's key, and giving it one for convenience
means a compromise of the database is a compromise of the edge.

> [!WARNING]
> `synqt/mesh/*.key` is git ignored by `synqt mesh init` and by the scaffolder. Check
> that it stayed ignored before your first push. A private key in git history is not
> removed by deleting the file, and the only real remedy is to issue a new authority and
> re-issue everything under it.

## Step 3: The certificate the browser wants

Get a certificate for your domain however you normally would. An ACME client such as
certbot or your host's built in one, a certificate your organisation issues, anything
that produces a full chain and a private key. SynQt has no opinion and no integration
here on purpose: certificate renewal is an operational concern with good tools already.

Put the two files where `synqt.production.yaml` says they are:

```text
gavel/
  certs/web/fullchain.pem
  certs/web/privkey.pem
```

Those paths are relative to the project root, like everything else an entity reads.

The alternative is to let something in front of the edge terminate TLS:

```yaml
# synqt.production.yaml, instead of the tls block
entities:
  - name: web
    public:
      tls_terminated_upstream: true
      origin: https://gavel.example.com
```

Then the edge listens plaintext on loopback and your reverse proxy owns the public
certificate. Both are supported; a release build refuses to guess between them.

> [!IMPORTANT]
> If a proxy is in front, do not let it rewrite response headers. The edge computes the
> Content-Security-Policy from your topology, including the exact `wss://` origin of the
> sync endpoint, and it emits the cross origin isolation headers when the client is built
> multi threaded. A proxy that helpfully replaces those breaks the client, and it breaks
> it in the browser rather than in your logs. [Content-Security-Policy](csp.md) has the
> detail.

## Step 4: The secrets, which are not certificates

The auction signs people in through GitHub, so the edge holds an OAuth client secret,
and the database holds nothing interesting yet. Neither goes in `synqt.yaml`.

A value that is a secret is declared as a reference:

```yaml
identity:
  providers:
    - name: github
      client_id: Iv1.0123456789abcdef
      client_secret: env:GITHUB_CLIENT_SECRET
```

and resolved at start from the entity's own env file:

```text
# web/.env on the edge host, readable only by the user the edge runs as
GITHUB_CLIENT_SECRET=the-real-value
```

Two rules are enforced rather than recommended, and they are worth knowing because they
change what mistakes are possible:

- A provider password or connection URI, and an identity provider's `client_secret`, are
  **rejected unless they are `env:` references**. You cannot paste a literal secret into
  the topology, so it cannot end up in the repository by being convenient.
- Any `env:` reference reachable from a client target is **rejected outright**. A secret
  cannot reach the browser by being named in the wrong section, because the section it
  would have to be named in is refused.

Each entity directory has an `.env.example` listing which names that entity expects. Copy
it to `.env` on the host, fill it in, and keep the file mode tight (`chmod 600`).

## Step 5: Watch it refuse

Certificates are the one thing `synqt check` deliberately does not enforce, because the
CA is not supposed to exist on a build machine. The check happens at start instead. See
it now, before it happens to you at three in the morning: move the database certificate
aside and try to start.

```cli
mv synqt/mesh/database.crt /tmp/
synqt serve --profile production
```

```text
error: entity "database" is configured for transport: mtls but has no certificate
       at synqt/mesh/database.crt
       issue one with: synqt mesh cert database
```

Put it back. That message is the whole of the design in one line: the failure names the
entity, names the file, and names the command, and it happens before anything listens on
a port.

## Try it, then think

> [!QUESTION]
> The edge and the database will run on the same host at first, to keep the first deploy
> simple. Mutual TLS on a loopback link seems like ceremony: nothing untrusted can reach
> `127.0.0.1`. Is there a way to turn it off, and should you?

<details class="solution" markdown>
<summary>Solution</summary>

There is, and it is `transport: local`, which swaps the TLS socket for a Unix domain
socket restricted to the user the entities run as. It is faster, it is a documented
option, and it is never chosen for you.

What it costs is the meaning of `Caller.entity`. On a local link the operating system
tells you which **user** connected, not which entity, so any process running as that
same user can claim to be the edge. The database's `Caller.entity === "web"` check
stops being an authentication and becomes an assumption about who else is on the box.
`synqt check` flags every local link for exactly that reason.

Mutual TLS on loopback costs a handshake per connection, which happens once per link and
not once per call. Take the ceremony. And when the database moves to its own host next
week, nothing about its trust position changes, which is the real payoff.

[The entity to entity links](security.md#the-entity-to-entity-links-the-mesh) has the
full comparison.

</details>

## Advice worth taking now

- **Put the expiry in a calendar.** Entity certificates are good for 398 days and the CA
  for twice that. `synqt mesh status` warns 30 days out, but only if somebody runs it. A
  reminder that fires a month before the first expiry costs nothing and saves an outage
  that will look, from the logs, like a networking fault.
- **Rotating an entity is easy. Do it that way.** `synqt mesh rotate database` issues a
  new leaf from the same authority; copy the new pair to that host and restart that one
  entity. Its peers verify against the CA certificate, which did not change, so nothing
  else needs to know.
- **Rotating the authority is a scheduled change.** Every entity trusts exactly one CA
  certificate, so there is no overlap period to hide behind: a new authority means new
  leaves everywhere and a coordinated restart. Plan it as a maintenance window rather
  than discovering it during one.
- **Never reuse the development CA.** `synqt dev` maintains a throwaway authority under
  `synqt/mesh/dev/` so development keeps mutual TLS with no setup. It is separate on
  purpose and a release build will not accept it.

Next: [Where the binaries go](tutorial-ship-hosts.md), and the shape that makes all of
these paths resolve.

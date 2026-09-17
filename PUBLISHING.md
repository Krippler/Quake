# Releases and images

## Tags

| | |
| --- | --- |
| `ghcr.io/krippler/quake:latest` | the newest release |
| `ghcr.io/krippler/quake:edge` | tracks `master` |
| `ghcr.io/krippler/quake:1.2.3` | a specific release |

`linux/amd64` only. The engine is an x86 software renderer built as portable C;
it would build for arm64, but nothing here has run on one.

## Cutting a release

1. Put the changes at the top of [CHANGELOG.md](CHANGELOG.md).
2. Tag: `git tag -a v1.2.3 -m 'Release 1.2.3: <what changed>' && git push --tags`.
3. The workflow in `.github/workflows/docker-publish.yml` builds, signs with
   cosign and pushes `latest` and the version tag.

The version is baked into the image as `QUAKE_VERSION`, printed at startup and
stamped into `play.html`. That matters more than it looks: everything that
decides whether sound arrives happens in the page, so a browser quietly running
a cached client from two releases ago looks identical from the container's
side. The start screen shows the client's stamp; the log shows the container's.

## Unraid

[`templates/unraid.xml`](templates/unraid.xml) is the Community Applications
template. `PUID`/`PGID` default to 99:100 there, which is what the appdata share
is owned by.

The one thing to get right in the template is the game data path: there is
nothing bundled, so a container with nothing mounted stops and says so.

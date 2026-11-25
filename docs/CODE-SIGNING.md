# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

## What is signed

Only the programs built from this repository's source by the release build on GitHub Actions
(`.github/workflows/build-windows.yml`): the launcher, its core and game starter, the update installer, the game
runtime and the Uncharted 2 / Uncharted 3 multiplayer import programs. `.signpath/artifact-configuration.xml` lists
them. Third-party files in the package (Qt, the MSYS2 runtime DLLs, FFmpeg, objdump) are never signed with this
certificate.

## Team roles

- **Committers and reviewers:** the members of the [UC-Resurrected](https://github.com/UC-Resurrected) GitHub
  organization with write access to this repository. Every change reaches `main` through them.
- **Approvers:** the organization's maintainers who are approvers in the SignPath project. Each signing request of a
  release is approved by one of them.

## How a release gets signed

Any organization member who can push to this repository makes a release the usual way (`tools/release.sh X.Y.Z
NOTES.md`). The tag starts the release build, which:

1. builds and packages the release,
2. sends the package folder to SignPath (a signing request, approved by an approver in SignPath),
3. zips the signed folder again and checks every listed program's signature,
4. publishes the GitHub release.

No one keeps the certificate or its key on their own PC: it stays with SignPath. While the SignPath token is not set
up, releases are published unsigned, as before; once it is set up, a release that cannot be signed is not published.

## Setting it up (once, by an organization owner)

1. Apply for the free open-source program at <https://signpath.org> with this repository.
2. In SignPath: add the project (slug `UCMR`), link GitHub as its trusted build system, paste
   `.signpath/artifact-configuration.xml` as the artifact configuration, and create the signing policy
   `release-signing`. Add every maintainer who should approve releases as an approver.
3. In GitHub, at the organization or repository level (Settings > Secrets and variables > Actions):
   - secret `SIGNPATH_API_TOKEN`: the API token of a SignPath CI user with submitter rights on the policy;
   - variable `SIGNPATH_ORGANIZATION_ID`: the SignPath organization id;
   - optional variables `SIGNPATH_PROJECT_SLUG` and `SIGNPATH_POLICY_SLUG` when they are not `UCMR` and
     `release-signing`.

## Privacy

This program will not transfer any information to other networked systems unless specifically requested by the
user or the person installing or operating it. What it does connect to (the update check on GitHub, the online
multiplayer services the player chooses to use) is described in the README.

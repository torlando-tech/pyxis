#!c=0
>Columba Downloads

`Bb6d*-----                                `b
`Bb6d      -----                 ---*--    `b
`Bb6d           -----         ---      --- `b
`Bb6d                -*--- ---            *`b
`Bb6d                     *                `b
`Bb6d                    /                 `b
`Bb6d                    /                 `b
`Bb6d                   /                  `b
`Bb6d                   /                  `b
`Bb6d                  /                   `b
`Bb6d                  /                   `b
`Bb6d                 /                    `b
`Bb6d                 /                    `b
`Bb6d                /                     `b
`Bb6d                /                     `b
`Bb6d               /                      `b
`Bb6d               *                      `b

`!Columba`!

A native android messaging application for the Reticulum Network Stack. Features a BLE interface

-

>>Latest Stable Release

`!v2.0.9`!
`F888Released: 2026-07-20`f

`F0af`[Download (27.1 MB)`:/page/download.mu`file=columba-2.0.9-EXPERIMENTAL-reticulum-kt-arm64-v8a-no-sentry.apk]`f  `F888(19 views)`f

`F666SHA256: d80e60b4954eadab9f79698bb8db53fe14e64ad94d390d5c98c678cfda8f2042`f

`!Release Notes:`!

## Columba 2.0.9

### Installation
Pick a **backend**, then download the APK matching your device's CPU architecture.

- **Python backend, RECOMMENDED** — ships Mark Qvist's official python reference implementations of Reticulum and LXMF via Chaquopy.
- **Kotlin backend** — ships Torlando's EXPERIMENTAL, AI-GENERATED native reticulum-kt / lxmf-kt / lxst-kt stack.

The python version is highly recommended. The kotlin version is a work in progress, and is not yet verified to be completely safe.
The kotlin version may yield better battery life depending on your device, and comes in a smaller apk due to not requiring a python runtime.

#### Python backend (`official-rns-py`)
| APK | Architecture | Devices |
|-----|-------------|---------|
| [`columba-2.0.9-official-rns-py-armeabi-v7a.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-official-rns-py-armeabi-v7a.apk) | armeabi-v7a | Older 32-bit ARM phones & tablets |
| [`columba-2.0.9-official-rns-py-arm64-v8a.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-official-rns-py-arm64-v8a.apk) | arm64-v8a | Most modern Android phones & tablets |
| [`columba-2.0.9-official-rns-py-x86_64.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-official-rns-py-x86_64.apk) | x86_64 | Chromebooks, emulators, some tablets |
| [`columba-2.0.9-official-rns-py-universal.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-official-rns-py-universal.apk) | All | Universal fallback (larger download) |

#### Kotlin backend (`EXPERIMENTAL-reticulum-kt`)
| APK | Architecture | Devices |
|-----|-------------|---------|
| [`columba-2.0.9-EXPERIMENTAL-reticulum-kt-armeabi-v7a.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-EXPERIMENTAL-reticulum-kt-armeabi-v7a.apk) | armeabi-v7a | Older 32-bit ARM phones & tablets |
| [`columba-2.0.9-EXPERIMENTAL-reticulum-kt-arm64-v8a.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-EXPERIMENTAL-reticulum-kt-arm64-v8a.apk) | arm64-v8a | Most modern Android phones & tablets |
| [`columba-2.0.9-EXPERIMENTAL-reticulum-kt-x86_64.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-EXPERIMENTAL-reticulum-kt-x86_64.apk) | x86_64 | Chromebooks, emulators, some tablets |
| [`columba-2.0.9-EXPERIMENTAL-reticulum-kt-universal.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-EXPERIMENTAL-reticulum-kt-universal.apk) | All | Universal fallback (larger download) |

**Telemetry variants:**
Each APK above also has a `-no-sentry` variant without crash reporting for maximum privacy — append `-no-sentry` to any download link above (e.g. the most common builds: [Python arm64-v8a no-sentry](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-official-rns-py-arm64-v8a-no-sentry.apk) · [Kotlin arm64-v8a no-sentry](https://github.com/torlando-tech/columba/releases/download/v2.0.9/columba-2.0.9-EXPERIMENTAL-reticulum-kt-arm64-v8a-no-sentry.apk)). Sentry collects stack traces when Columba encounters errors
and provides helpful information to the developer to improve Columba, but is hosted at sentry.io, and is thus not the maximum privacy option.

> **Not sure which to pick?** Most Android phones use **arm64-v8a**. If unsure, use the **universal** APK.

### Verification
See [SECURITY.md](https://github.com/torlando-tech/columba/blob/main/SECURITY.md) for verification instructions.

**Signing Certificate Fingerprints:**
```
SHA-256: 02:2B:12:20:48:63:A3:1F:BF:07:5B:C9:F9:34:1E:33:52:78:80:2E:80:C9:27:A4:75:46:E4:7E:2F:4A:0C:5F
SHA-1:   0A:6B:AE:58:4E:D7:B5:D0:35:8B:3C:7B:65:11:D6:3A:81:21:0D:CE
```

**SHA256 Checksums:**
```
d80e60b4954eadab9f79698bb8db53fe14e64ad94d390d5c98c678cfda8f2042  columba-2.0.9-EXPERIMENTAL-reticulum-kt-arm64-v8a-no-sentry.apk
10dff623760088c00fea95bbdcd32aa92d96cb6f2e6d08e9e148d51db717c859  columba-2.0.9-EXPERIMENTAL-reticulum-kt-arm64-v8a.apk
b8d5b94c5e5a4f535bffad9c3d92ade83caf9de7392543781c1f2a8de2ccd1d5  columba-2.0.9-EXPERIMENTAL-reticulum-kt-armeabi-v7a-no-sentry.apk
cba52a852412592e4fa69fd7942f1def6b4377332addc46d3a8158f522b7b8d4  columba-2.0.9-EXPERIMENTAL-reticulum-kt-armeabi-v7a.apk
77c5df7c2521436ac3dddda43341d8ab537da9a9edd495174ce976e8d3df8e07  columba-2.0.9-EXPERIMENTAL-reticulum-kt-universal-no-sentry.apk
bafb1cd3c9693c94c3fb672a7536e126457450b0ab9225d853d2cbcf9591709e  columba-2.0.9-EXPERIMENTAL-reticulum-kt-universal.apk
f2231ababe6fe2a6e59da846546c858804165465f52a572f00551d7e562a3ccf  columba-2.0.9-EXPERIMENTAL-reticulum-kt-x86_64-no-sentry.apk
5a39564a4cc592d60961ae0fb7329f2754e426b91c3e858ee5439f0e87a059e5  columba-2.0.9-EXPERIMENTAL-reticulum-kt-x86_64.apk
4db07f32d3f4f042b0ff0510c39350e032615fab6c8cd7114cb1ebedb6f5e143  columba-2.0.9-official-rns-py-arm64-v8a-no-sentry.apk
c29392e074d0aa9e96c902c24a3e525d83abff04e1ec21cbb8055fa7aa7bad4a  columba-2.0.9-official-rns-py-arm64-v8a.apk
1f2cfede410e82873351f4f66046b3a34e2a26a7ad9cb26ad74b5c0f43fa7331  columba-2.0.9-official-rns-py-armeabi-v7a-no-sentry.apk
9d1c73153f5d80655eb99f418889337942536d3e76914c0f204b8494835f482d  columba-2.0.9-official-rns-py-armeabi-v7a.apk
a58ef772dc1fcc0a40a78b30515640c30b229f89c845170ee30589dea8b8b876  columba-2.0.9-official-rns-py-universal-no-sentry.apk
e4ec9a9342d4fb6f730db06c81b99229c63bd6f4d9da93ccca73e3419a564546  columba-2.0.9-official-rns-py-universal.apk
c037a4b312483f0f4deedee4481c8ffa38aae00a0c50bfe992b8b05ddf46d093  columba-2.0.9-official-rns-py-x86_64-no-sentry.apk
922bb1d06057ba51fc0af665422ad5302f42297d682d62aece30f5308b8e24de  columba-2.0.9-official-rns-py-x86_64.apk

```



## What's Changed
* deps: bump Chaquopy 16.1.0 → 17.0.0 for Gradle 9 compat by @torlando-tech in https://github.com/torlando-tech/columba/pull/643
* feat(map): declutter overlapping contact markers by @MatthieuTexier in https://github.com/torlando-tech/columba/pull/572
* feat: add background location permission flow in Location Sharing settings by @MatthieuTexier in https://github.com/torlando-tech/columba/pull/600
* feat: add 'Locate on Map' to Chats, Messaging, and Contacts screens by @MatthieuTexier in https://github.com/torlando-tech/columba/pull/597
* fix: prevent binder buffer exhaustion causing crash loop by @torlando-tech in https://github.com/torlando-tech/columba/pull/652
* feat: add offline mode banner (#623) by @torlando-tech in https://github.com/torlando-tech/columba/pull/626
* fix: show notifications when app backgrounded on active conversation by @torlando-tech in https://github.com/torlando-tech/columba/pull/649
* fix: sort messages by local receive time to prevent clock-drift reordering by @torlando-tech in https://github.com/torlando-tech/columba/pull/645
* ci: split module tests into separate job to prevent shard 0 timeouts by @torlando-tech in https://github.com/torlando-tech/columba/pull/655
* Prevent duplicate message notifications on service restart by @torlando-tech in https://github.com/torlando-tech/columba/pull/650
* feat: add block & blackhole peer protection by @torlando-tech in https://github.com/torlando-tech/columba/pull/601
* feat: clean up stale announces, add PHONE NodeType and cross-link buttons by @torlando-tech in https://github.com/torlando-tech/columba/pull/581
* feat: add WiFi hotspot sharing for APK distribution without existing network by @torlando-tech in https://github.com/torlando-tech/columba/pull/632
* ci: trigger CI on release/* branches by @torlando-tech in https://github.com/torlando-tech/columba/pull/660
* fix: catch SecurityException when location permission revoked while backgrounded by @torlando-tech in https://github.com/torlando-tech/columba/pull/662
* feat: multiple firmware sources + custom firmware in RNode flasher (#485) by @torlando-tech in https://github.com/torlando-tech/columba/pull/582
* feat: RNode disconnect notification by @torlando-tech in https://github.com/torlando-tech/columba/pull/628
* fix: path persistence + proactive path resolution by @torlando-tech in https://github.com/torlando-tech/columba/pull/665
* chore: bump GitHub Actions dependencies to latest versions by @torlando-tech in https://github.com/torlando-tech/columba/pull/667
* feat: show outbound interface in message details (#646) by @torlando-tech in https://github.com/torlando-tech/columba/pull/666
* feat: NomadNet browser with Micron rendering, URL bar, and URI handler by @torlando-tech in https://github.com/torlando-tech/columba/pull/671
* ci: Bump reactivecircus/android-emulator-runner from 2.36.0 to 2.37.0 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/682
* fix: prevent crash on corrupted interface in edit dialog by @torlando-tech in https://github.com/torlando-tech/columba/pull/710
* fix: replace modal restart dialog with inline banner by @torlando-tech in https://github.com/torlando-tech/columba/pull/711
* fix: preserve settings state when combine flow re-fires (#688) by @torlando-tech in https://github.com/torlando-tech/columba/pull/712
* feat: add IFAC network_name and passphrase support for RNode interfaces by @torlando-tech in https://github.com/torlando-tech/columba/pull/718
* fix: prevent identity file loss and add opportunistic key backup by @MatthieuTexier in https://github.com/torlando-tech/columba/pull/717
* test: add regression tests for identity recovery edge cases by @torlando-tech in https://github.com/torlando-tech/columba/pull/720
* fix: prevent initial camera from overriding Locate on Map by @MatthieuTexier in https://github.com/torlando-tech/columba/pull/715
* fix: show address for auto-discovered backbone interfaces by @torlando-tech in https://github.com/torlando-tech/columba/pull/722
* fix: handle inline field=value variables in Micron links by @torlando-tech in https://github.com/torlando-tech/columba/pull/723
* feat: improve NomadNet page request reliability and status reporting by @torlando-tech in https://github.com/torlando-tech/columba/pull/724
* feat: add NomadNet file download support by @torlando-tech in https://github.com/torlando-tech/columba/pull/725
* Add micron parser audit comparing Columba against NomadNet and micron-parser-js by @torlando-tech in https://github.com/torlando-tech/columba/pull/678
* fix: prevent stale page request from overwriting current page by @torlando-tech in https://github.com/torlando-tech/columba/pull/726
* feat: render discovered interfaces as map pins with type filtering by @torlando-tech in https://github.com/torlando-tech/columba/pull/729
* fix: prevent announce list scroll jumps when new announces arrive by @torlando-tech in https://github.com/torlando-tech/columba/pull/727
* chore: upgrade to Gradle 9.4.1 and AGP 9.1.0 by @torlando-tech in https://github.com/torlando-tech/columba/pull/730
* fix: deduplicate path requests with hasPath guard by @torlando-tech in https://github.com/torlando-tech/columba/pull/735
* ci: bump gradle/actions from 5 to 6 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/731
* deps(deps): Bump the compose group with 2 updates by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/684
* deps(deps): Bump cameraX from 1.5.2 to 1.5.3 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/685
* ci: bump codecov/codecov-action from 5 to 6 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/732
* fix: scope MapViewModel announce query to location senders by @torlando-tech in https://github.com/torlando-tech/columba/pull/737
* Interface discovery: search + filter + IFAC handling by @torlando-tech in https://github.com/torlando-tech/columba/pull/772
* Fix/sentry gradle hang by @torlando-tech in https://github.com/torlando-tech/columba/pull/776
* Complete native Reticulum migration and remove Python runtime by @torlando-tech in https://github.com/torlando-tech/columba/pull/762
* ci: bump softprops/action-gh-release from 2 to 3 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/768
* ci: bump actions/github-script from 8 to 9 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/769
* deps(deps): bump com.squareup.leakcanary:leakcanary-android from 2.13 to 2.14 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/756
* Default transport node to disabled for new users by @torlando-tech in https://github.com/torlando-tech/columba/pull/777
* Keep foreground notification in sync after :reticulum process restart by @torlando-tech in https://github.com/torlando-tech/columba/pull/775
* deps(deps): bump org.robolectric:robolectric from 4.16 to 4.16.1 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/755
* Rename com.lxmf.messenger -> network.columba.app by @torlando-tech in https://github.com/torlando-tech/columba/pull/779
* Remove AIDL surface from ReticulumService by @torlando-tech in https://github.com/torlando-tech/columba/pull/780
* Remove Python-era dead code from ReticulumService by @torlando-tech in https://github.com/torlando-tech/columba/pull/781
* Collapse Room migrations — ColumbaDatabase v1 + InterfaceDatabase v1 by @torlando-tech in https://github.com/torlando-tech/columba/pull/782
* Make builds reproducible by @torlando-tech in https://github.com/torlando-tech/columba/pull/783
* Consume Reticulum/LXMF/LXST stack from JitPack (drop submodules) by @torlando-tech in https://github.com/torlando-tech/columba/pull/786
* Wire delivery + transport identities through memory, scrub on-disk copies by @torlando-tech in https://github.com/torlando-tech/columba/pull/785
* Bump LXST-kt to v0.0.3 (JVM 21 target) by @torlando-tech in https://github.com/torlando-tech/columba/pull/787
* Drop files/reticulum/ backup exclusion (take 2, with LXMF ratchets) by @torlando-tech in https://github.com/torlando-tech/columba/pull/790
* About card: AGPLv3 label, catalog-driven protocol versions, settings-flow state preservation by @torlando-tech in https://github.com/torlando-tech/columba/pull/789
* Eliminate plaintext identity-file window in createIdentity/importIdentity paths by @torlando-tech in https://github.com/torlando-tech/columba/pull/791
* Bump LXMF-kt to v0.0.4 by @torlando-tech in https://github.com/torlando-tech/columba/pull/793
* Register MIGRATION_2_3 + destinationRatchetStore in NativeReticulumProtocol by @torlando-tech in https://github.com/torlando-tech/columba/pull/794
* Update and rename LICENSE to LICENSE.md by @serialrf433 in https://github.com/torlando-tech/columba/pull/797
* Route to IdentityUnlockScreen when Keystore-wrapped identity can't be decrypted after restore by @torlando-tech in https://github.com/torlando-tech/columba/pull/796
* fix: harden Room DB against process-kill corruption (COLUMBA-8C) by @torlando-tech in https://github.com/torlando-tech/columba/pull/799
* fix: ignore CancellationException in rapid interface toggles by @torlando-tech in https://github.com/torlando-tech/columba/pull/801
* fix: remove TestHostActivity launcher entry and LeakCanary icon by @torlando-tech in https://github.com/torlando-tech/columba/pull/743
* fix: route all PRAGMAs through db.query() so Android accepts them by @torlando-tech in https://github.com/torlando-tech/columba/pull/803
* fix: pass IFAC passphrase through to TCPClientInterface factory by @torlando-tech in https://github.com/torlando-tech/columba/pull/804
* fix: render image/file/audio-only messages instead of skipping as telemetry by @torlando-tech in https://github.com/torlando-tech/columba/pull/805
* fix: render file attachments sent in LXMF positional wire format by @torlando-tech in https://github.com/torlando-tech/columba/pull/806
* Bump reticulum-kt to v0.0.8 by @torlando-tech in https://github.com/torlando-tech/columba/pull/808
* Surface announce filters as sticky chip rows; rebrand Nodes → Sites by @torlando-tech in https://github.com/torlando-tech/columba/pull/807
* fix: guard PRAGMA journal_mode and synchronous against Room transactions by @sentry[bot] in https://github.com/torlando-tech/columba/pull/809
* Fix ANR when answering LXST call on slow transports by @torlando-tech in https://github.com/torlando-tech/columba/pull/811
* Restore long-press → delete on interface cards by @torlando-tech in https://github.com/torlando-tech/columba/pull/812
* Refresh interface online status when library state changes by @torlando-tech in https://github.com/torlando-tech/columba/pull/813
* Stop interface-card subtitle from repeating the type label by @torlando-tech in https://github.com/torlando-tech/columba/pull/814
* Bump reticulum-kt to v0.0.10 by @torlando-tech in https://github.com/torlando-tech/columba/pull/815
* NativeInterfaceFactory owns its own coroutine scope by @torlando-tech in https://github.com/torlando-tech/columba/pull/818
* Plumb received-side RSSI/SNR/interface/hop fields to MessageEntity by @torlando-tech in https://github.com/torlando-tech/columba/pull/819
* Propagate LXMessage packet metadata through the receive pipeline by @torlando-tech in https://github.com/torlando-tech/columba/pull/820
* Bump reticulum-kt to v0.0.11 (BLE per-packet RSSI) + local composite-build dev loop by @torlando-tech in https://github.com/torlando-tech/columba/pull/826
* Remove .drop(1) from interface online observer to close fast-transition race by @torlando-tech in https://github.com/torlando-tech/columba/pull/827
* Bump reticulum-kt to v0.0.12 (expire path on link establishment failure) by @torlando-tech in https://github.com/torlando-tech/columba/pull/828
* Dispatch MapViewModel.loadInterfaceMarkers to IO to avoid main-thread Room read by @torlando-tech in https://github.com/torlando-tech/columba/pull/829
* Bump LXMF-kt to v0.0.6 (send-hang fix) + dependencySubstitution for JitPack-collapsed libs by @torlando-tech in https://github.com/torlando-tech/columba/pull/830
* Add Edit action to interface-card long-press menu by @torlando-tech in https://github.com/torlando-tech/columba/pull/831
* Give interface cards the same semantic icon mapping as announce cards by @torlando-tech in https://github.com/torlando-tech/columba/pull/832
* Unify IFAC network-access UI across all interface wizards by @torlando-tech in https://github.com/torlando-tech/columba/pull/833
* Implement TCPServer branch in NativeInterfaceFactory by @torlando-tech in https://github.com/torlando-tech/columba/pull/834
* Bump reticulum-kt to v0.0.13 (BLE handshake concurrency fix) by @torlando-tech in https://github.com/torlando-tech/columba/pull/835
* Move map interface filters behind a layers icon by @torlando-tech in https://github.com/torlando-tech/columba/pull/838
* Add map style picker (Auto / Light / Dark) to the layers sheet by @torlando-tech in https://github.com/torlando-tech/columba/pull/839
* Bump LXMF-kt to v0.0.7 (processingScope silent-drop fix) by @torlando-tech in https://github.com/torlando-tech/columba/pull/840
* Render Micron links on form lines in the NomadNet browser by @torlando-tech in https://github.com/torlando-tech/columba/pull/841
* Auto-linkify bare http/https URLs in the NomadNet browser by @torlando-tech in https://github.com/torlando-tech/columba/pull/842
* fix: prevent spontaneous scroll jumps when reading old messages by @torlando-tech in https://github.com/torlando-tech/columba/pull/668
* fix: replace continuous GPS with on-demand one-shot in Group Tracker by @MatthieuTexier in https://github.com/torlando-tech/columba/pull/746
* Move Transport Node toggle to a new Advanced settings card by @torlando-tech in https://github.com/torlando-tech/columba/pull/846
* Bump LXMF-kt to v0.0.8 — DIRECT double-delivery + dedup keys fixes by @torlando-tech in https://github.com/torlando-tech/columba/pull/848
* Bump reticulum-kt v0.0.14 + LXMF-kt v0.0.9 — multi-hop link race fixes + version alignment by @torlando-tech in https://github.com/torlando-tech/columba/pull/849
* detekt: add DiscardedConcurrencyReturn custom rule by @torlando-tech in https://github.com/torlando-tech/columba/pull/858
* feat(NomadNetBrowserScreen): replace duplicate Identify dropdown item with Close site by @torlando-tech in https://github.com/torlando-tech/columba/pull/882
* chore: bump reticulum-kt v0.0.14 → v0.0.16, LXMF-kt v0.0.9 → v0.0.11 by @torlando-tech in https://github.com/torlando-tech/columba/pull/870
* chore(build): isolate debug builds with .debug applicationId + columbatest label by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/885
* fix(SmokeTest): assert context.packageName == BuildConfig.APPLICATION_ID by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/895
* fix(OnboardingPagerScreen): refresh permission cards on resume by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/890
* feat(InterfaceConfig): per-interface Wi-Fi/cellular network restriction by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/896
* fix(viewmodel): deflake DebugViewModelEventDrivenTest + ApkSharingViewModelTest by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/891
* feat(MicronPageContent): allow text selection on rendered NomadNet pages by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/884
* Update ci.yml - update JDK to 25 and setup-java to `@5` by @serialrf433 in https://github.com/torlando-tech/columba/pull/778
* ci(release): include reticulum-kt / LXMF-kt / LXST-kt changelogs in release notes by @torlando-tech in https://github.com/torlando-tech/columba/pull/908
* test(debug): TestReceiver/TestController for phone harness Stage 1 by @torlando-tech in https://github.com/torlando-tech/columba/pull/909
* deps(reticulum-kt, LXMF-kt): bump v0.0.16 → v0.0.17, v0.0.11 → v0.0.12 by @torlando-tech in https://github.com/torlando-tech/columba/pull/910
* Fix shutdown race that could close DB while Room writes are mid-transaction (Sentry COLUMBA-8R) by @torlando-tech in https://github.com/torlando-tech/columba/pull/857
* deps(LXMF-kt): bump v0.0.12 → v0.0.13 (Sideband stamp-drop fix) by @torlando-tech in https://github.com/torlando-tech/columba/pull/914
* deps(reticulum-kt): bump v0.0.17 → v0.0.20 by @torlando-tech in https://github.com/torlando-tech/columba/pull/915
* fix(nomadnet): submit field defaults and pad form for IME (#917) by @torlando-tech in https://github.com/torlando-tech/columba/pull/918
* feat(rns): dual-backend architecture (kotlin + python flavors) by @torlando-tech in https://github.com/torlando-tech/columba/pull/932
* feat: LXST call privacy gates + telemetry responder (dual-build) by @torlando-tech in https://github.com/torlando-tech/columba/pull/933
* feat: punch-list items 3-7 + 10 + identity flow + RNode (dual-build) by @torlando-tech in https://github.com/torlando-tech/columba/pull/934
* fix(python-backend): map shows no discovered-interface markers by @torlando-tech in https://github.com/torlando-tech/columba/pull/935
* chore(license): AGPLv3 -> MPL 2.0 for Reticulum compatibility by @torlando-tech in https://github.com/torlando-tech/columba/pull/936
* Dual-backend release: Python-first APKs, co-installable Kotlin flavor, R8 bridge gate by @torlando-tech in https://github.com/torlando-tech/columba/pull/938
* feat(rns,ui): user-toggleable Share Instance hosting (Python backend) by @torlando-tech in https://github.com/torlando-tech/columba/pull/939
* fix(release): keep minified pythonBackend Python startup working (duplicate org.json) by @torlando-tech in https://github.com/torlando-tech/columba/pull/940
* feat(ble): surface live BLE peer connections to the Network Status card by @torlando-tech in https://github.com/torlando-tech/columba/pull/941
* feat(settings): add BLE Connections button to the Network card by @torlando-tech in https://github.com/torlando-tech/columba/pull/942
* ci(release): bump release build timeout 20m → 40m by @torlando-tech in https://github.com/torlando-tech/columba/pull/943
* fix(rns): TCP/interface type shows as obfuscated "wy2" in release builds by @torlando-tech in https://github.com/torlando-tech/columba/pull/946
* feat(telemetry): consent-based crash reporting + build-time Sentry removal by @torlando-tech in https://github.com/torlando-tech/columba/pull/947
* ci(release): link APKs directly from the release body tables by @torlando-tech in https://github.com/torlando-tech/columba/pull/948
* fix(rns): inbound LXST voice R8 regression + enforce @ReflectivelyKept keep contract by @torlando-tech in https://github.com/torlando-tech/columba/pull/949
* fix(ci): raise Gradle heap to 6GB to stop release APK packaging OOM by @torlando-tech in https://github.com/torlando-tech/columba/pull/950
* fix(rns-host): 1-arg readParcelable in snapshot reader (API 33 NoSuchMethodError on minSdk 24) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/953
* chore(lint): enable Android Lint (NewApi, no baseline) + fix 6 NewApi violations by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/954
* feat(reactions): migrate reaction wire format to canonical LXMF FIELD_REACTION (0x40) by @torlando-tech in https://github.com/torlando-tech/columba/pull/956
* fix(settings): don't crash on RnsException(BackendNotReady) during availability polling by @torlando-tech in https://github.com/torlando-tech/columba/pull/960
* ci: Bump actions/setup-python from 5 to 6 by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/959
* fix(reactions): reject blank/absent emoji on the legacy 0x10 parse path by @torlando-tech in https://github.com/torlando-tech/columba/pull/957
* fix(rns-ipc): guard network-status observer registration against dead backend by @torlando-tech in https://github.com/torlando-tech/columba/pull/962
* fix(rns-ipc): guard all observer registrations against dead backend (COLUMBA-B0) by @torlando-tech in https://github.com/torlando-tech/columba/pull/963
* fix(proguard): keep LXST NativeCodec2/NativeOpus for JNI RegisterNatives (COLUMBA-B1/B2) by @torlando-tech in https://github.com/torlando-tech/columba/pull/964
* chore(deps): bump LXST-kt to v0.0.4 (JNI keep for NativeCodec2/NativeOpus) by @torlando-tech in https://github.com/torlando-tech/columba/pull/966
* fix(rns-ipc): transfer LXMF attachments out-of-band to fix TransactionTooLargeException by @torlando-tech in https://github.com/torlando-tech/columba/pull/967
* fix(performance): batch interface_first_seen inserts in MapViewModel by @sentry[bot] in https://github.com/torlando-tech/columba/pull/951
* fix(build): enable java.time desugaring for minSdk 24 by @sentry[bot] in https://github.com/torlando-tech/columba/pull/944
* Match Sideband's LXMF telemetry-request command: epoch-seconds timebase + collector flag (#927) by @torlando-tech in https://github.com/torlando-tech/columba/pull/968
* fix(settings): handle ActivityNotFoundException when opening URLs by @sentry[bot] in https://github.com/torlando-tech/columba/pull/958
* fix(ui): treat NomadNet page addresses in messages as in-app links by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/971
* fix(ui): make announce-stream filter chips exclusive (closes #862) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/975
* feat(ui): collapsible announce-stream filter chips + WindowSizeClass foundation (closes #922) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/976
* fix(announce,ui): make announce-card interface icon resilient to classifier-cache drift by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/978
* fix(rns-ipc): inbound attachments out-of-band + stop observer-detach cascade (TransactionTooLargeException) by @torlando-tech in https://github.com/torlando-tech/columba/pull/979
* feat(share-instance): surface host row + Hub icon, fix Change-pending hint by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/974
* fix(share-instance): persist hosting toggle + close 37428 listener so hosting survives :reticulum restart by @torlando-tech in https://github.com/torlando-tech/columba/pull/980
* build: bump ktlint to 1.5.0 to relax discouraged-comment-location (#923) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/984
* Fix: pan short NomadNet pages from anywhere in the viewport (#681) by @torlando-tech in https://github.com/torlando-tech/columba/pull/982
* fix(location): restore precise location for sharing — drop merged FINE maxSdkVersion cap (#855) by @torlando-tech in https://github.com/torlando-tech/columba/pull/986
* feat(messaging): add "Select text" action to copy message substrings (#920) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/985
* feat(nomadnet): remember selected page rendering mode across sessions by @torlando-tech in https://github.com/torlando-tech/columba/pull/983
* fix(location): gate precise-permission prompt on location sharing enabled (#991) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/992
* fix(announce): route test announce through proper LXMF self-announce (#988) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/989
* deps(deps): Bump the kotlin group across 1 directory with 6 updates by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/913
* fix(identity): reject non-64-byte keys before RNS reconstruction (COLUMBA-B8) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/993
* chore(deps): bump reticulum-kt v0.0.20 → v0.0.21 (COLUMBA-B7) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/994
* fix(nomadnet): handle link field/query params on deep-link & URL-bar page loads by @torlando-tech in https://github.com/torlando-tech/columba/pull/998
* fix(qr): don't crash when CameraX init fails on virtual cameras (COLUMBA-BA) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/1000
* feat(messaging): send on Enter, Shift+Enter for newline by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/999
* deps(deps): bump the compose group with 2 updates by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/995
* feat(InterfaceManagementScreen): network-restriction chip on interface cards by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/906

## New Contributors
* @torlando-agent[bot] made their first contribution in https://github.com/torlando-tech/columba/pull/885

**Full Changelog**: https://github.com/torlando-tech/columba/compare/v0.9.18...v2.0.9

-

>>Latest Pre-release

`*v2.0.9-beta`*
`F888Released: 2026-07-15`f

`Ff80`[Download Beta (27.1 MB)`:/page/download.mu`file=columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-arm64-v8a-no-sentry.apk]`f  `F888(2 views)`f

`!Release Notes:`!

## Columba 2.0.9-beta

### Installation
Pick a **backend**, then download the APK matching your device's CPU architecture.

- **Python backend, RECOMMENDED** — ships Mark Qvist's official python reference implementations of Reticulum and LXMF via Chaquopy.
- **Kotlin backend** — ships Torlando's EXPERIMENTAL, AI-GENERATED native reticulum-kt / lxmf-kt / lxst-kt stack.

The python version is highly recommended. The kotlin version is a work in progress, and is not yet verified to be completely safe.
The kotlin version may yield better battery life depending on your device, and comes in a smaller apk due to not requiring a python runtime.

#### Python backend (`official-rns-py`)
| APK | Architecture | Devices |
|-----|-------------|---------|
| [`columba-2.0.9-beta-official-rns-py-armeabi-v7a.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-official-rns-py-armeabi-v7a.apk) | armeabi-v7a | Older 32-bit ARM phones & tablets |
| [`columba-2.0.9-beta-official-rns-py-arm64-v8a.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-official-rns-py-arm64-v8a.apk) | arm64-v8a | Most modern Android phones & tablets |
| [`columba-2.0.9-beta-official-rns-py-x86_64.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-official-rns-py-x86_64.apk) | x86_64 | Chromebooks, emulators, some tablets |
| [`columba-2.0.9-beta-official-rns-py-universal.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-official-rns-py-universal.apk) | All | Universal fallback (larger download) |

#### Kotlin backend (`EXPERIMENTAL-reticulum-kt`)
| APK | Architecture | Devices |
|-----|-------------|---------|
| [`columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-armeabi-v7a.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-armeabi-v7a.apk) | armeabi-v7a | Older 32-bit ARM phones & tablets |
| [`columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-arm64-v8a.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-arm64-v8a.apk) | arm64-v8a | Most modern Android phones & tablets |
| [`columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-x86_64.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-x86_64.apk) | x86_64 | Chromebooks, emulators, some tablets |
| [`columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-universal.apk`](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-universal.apk) | All | Universal fallback (larger download) |

**Telemetry variants:**
Each APK above also has a `-no-sentry` variant without crash reporting for maximum privacy — append `-no-sentry` to any download link above (e.g. the most common builds: [Python arm64-v8a no-sentry](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-official-rns-py-arm64-v8a-no-sentry.apk) · [Kotlin arm64-v8a no-sentry](https://github.com/torlando-tech/columba/releases/download/v2.0.9-beta/columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-arm64-v8a-no-sentry.apk)). Sentry collects stack traces when Columba encounters errors
and provides helpful information to the developer to improve Columba, but is hosted at sentry.io, and is thus not the maximum privacy option.

> **Not sure which to pick?** Most Android phones use **arm64-v8a**. If unsure, use the **universal** APK.

### Verification
See [SECURITY.md](https://github.com/torlando-tech/columba/blob/main/SECURITY.md) for verification instructions.

**Signing Certificate Fingerprints:**
```
SHA-256: 02:2B:12:20:48:63:A3:1F:BF:07:5B:C9:F9:34:1E:33:52:78:80:2E:80:C9:27:A4:75:46:E4:7E:2F:4A:0C:5F
SHA-1:   0A:6B:AE:58:4E:D7:B5:D0:35:8B:3C:7B:65:11:D6:3A:81:21:0D:CE
```

**SHA256 Checksums:**
```
1ce3310b1bd860d2c99d21e94654be99da2c56d06227be3a80acf42764067cfa  columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-arm64-v8a-no-sentry.apk
54c07d095504887ce1820cdc139080a71aac021c0319ccedf9b2a425243a68b1  columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-arm64-v8a.apk
b68c54c8c955e4b259c683293ecaaf8e895927f396ff662d7cbaa9739628df60  columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-armeabi-v7a-no-sentry.apk
b2f0dd58d4972f11009aa371e7e3e33b9116c4757f5014f63844c0c31e675a1d  columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-armeabi-v7a.apk
61ad470036ac73fba64f508e3bf56e864f6a20cb94e31bdfe224c0c2695187c4  columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-universal-no-sentry.apk
e72041565fbba312b87a8cc77c0073def6d84a0d0492a2442b414f2c44f8da74  columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-universal.apk
60f5d2e13bf1a531678ab5c26e5c8e1c2a3f8e94cdafda0d6a91c4284ae08535  columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-x86_64-no-sentry.apk
30d7eb35ccf239c7c76b00c29407bf552c553e79951b5ebe007518d2d69230ac  columba-2.0.9-beta-EXPERIMENTAL-reticulum-kt-x86_64.apk
0d263e1b28c54dfd57ca107fd293d4d660b047c567ee9bf067aab55eb00b47eb  columba-2.0.9-beta-official-rns-py-arm64-v8a-no-sentry.apk
beb2bb78e18424097aa5efa8f72971abd183622528b1db6249987eb6d4f22e10  columba-2.0.9-beta-official-rns-py-arm64-v8a.apk
9e95d43065a2a7159d5181a8e0a69a3c303d7d35395b82932a46dccbb93c991c  columba-2.0.9-beta-official-rns-py-armeabi-v7a-no-sentry.apk
85572f1108332c5691f64331741b8b19bc94c3d366b181aabf23c51c68a6a8c4  columba-2.0.9-beta-official-rns-py-armeabi-v7a.apk
9cf43b6e9178f049822f9b92bda5530714a793252ab856bd26a2d68e400f795b  columba-2.0.9-beta-official-rns-py-universal-no-sentry.apk
dace04b6ab19a016f2cf9a88cf45fdba3b8153bf233ebfcc62f8a3dc8b53b259  columba-2.0.9-beta-official-rns-py-universal.apk
cba20c6d247ed25851f4017d5fe66d008dc8dc21f55ec59247511884765e9f7b  columba-2.0.9-beta-official-rns-py-x86_64-no-sentry.apk
a2f61cb699424978abbe0d33d615f61e143b7943903855e94d109e4e0f7e14e1  columba-2.0.9-beta-official-rns-py-x86_64.apk

```



## What's Changed
* fix(qr): don't crash when CameraX init fails on virtual cameras (COLUMBA-BA) by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/1000
* feat(messaging): send on Enter, Shift+Enter for newline by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/999
* deps(deps): bump the compose group with 2 updates by @dependabot[bot] in https://github.com/torlando-tech/columba/pull/995
* feat(InterfaceManagementScreen): network-restriction chip on interface cards by @torlando-agent[bot] in https://github.com/torlando-tech/columba/pull/906


**Full Changelog**: https://github.com/torlando-tech/columba/compare/v2.0.8-beta...v2.0.9-beta

-

`F0af`[View All Releases`:/page/releases.mu]`f

-

`F666Last synced: 2026-08-04`f

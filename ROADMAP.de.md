# WARP-Roadmap

Stand: 0.4.6

Auch auf [English](ROADMAP.md) und [Русский](ROADMAP.ru.md).

Die Etappen gehen der Reihe nach: `[x]` ist erledigt, `[ ]` ist geplant. Die aktuelle Etappe ist die erste unfertige.

Entwurf für Repositorys und Knoten: [docs/NODES.de.md](docs/NODES.de.md).

## Der grundlegende Paketmanager
- [x] Pakete installieren, entfernen, auflisten und ansehen
- [x] Versionierter lokaler Speicher mit sofortigem Rollback
- [x] Suche über die verfügbaren Pakete
- [x] `.warp`-Archive bauen (`warp pack`)

## Vertrauensmodell
- [x] Nur einem festgelegten Ed25519-Schlüssel vertrauen
- [x] Signierter Index, zusammen mit seiner Signatur vom selben Spiegel geholt
- [x] sha256-Prüfung jedes Archivs und Deltas gegen den Index
- [x] Schlüsselerzeugung und Signieren (`warp keygen`, `warp sign`)

## Repositorys und Aktualisierungen
- [x] Mehrere Repositorys mit festgelegten Schlüsseln
- [x] `warp upgrade` für installierte Pakete
- [x] Binäre Delta-Updates (Format WARPDLT1)
- [x] Abhängigkeiten im signierten Index (nach Version festgelegt)
- [x] Freiwilliges Verteilen (`warp seed`, `warp volunteer`) mit Platzquote und Monatslimit für den Upload

## Auf Distributionen getestet
- [x] Delta-Tests und ein E2E-Test install → upgrade → rollback
- [x] CI auf K1OS, Alpine, AlmaLinux, Arch, Artix, Devuan, Fedora, Ubuntu, Void und Void musl

## Schutz vor Rollback
- [ ] Eine Versionsnummer und ein Ablaufdatum für den Index
- [ ] Der Client lehnt einen Index ab, der älter ist als der bereits gesehene, oder einen abgelaufenen

## Repository-Modus
- [ ] `warp repo init` und `warp publish` statt eines externen Skripts
- [ ] Release-Hinweise beim Veröffentlichen einer Version (`warp publish --notes`), im signierten Index gespeichert
- [ ] Getrennte Schlüssel: Root (offline), Index und Online

## Knoten-Modus
- [ ] Knotenzertifikate mit Ablaufdatum, vom Repository signiert
- [ ] Knoten treten im beiderseitigen Einvernehmen bei und synchronisieren alle Pakete oder einen Teil davon
- [ ] Automatische Erneuerung, Verfügbarkeitsprüfungen und Widerruf von Knoten
- [ ] Der Client lädt von Knoten und wechselt bei Ausfall
- [ ] Namensraum-Delegation an Knoten (falls nötig)

## Weitere Architekturen
Alle Pakete werden heute für x86_64 gebaut, und die Architektur steckt nur im Archivnamen.
- [x] Plattform im Index (`builds` pro `<os>-<arch>`): Der Client nimmt nur den Build für sein eigenes Betriebssystem und seine CPU und kompiliert nie
- [ ] Pakete und CI für aarch64 (WARP baut sich selbst und besteht seine Tests unter linux/aarch64 in einer OrbStack-Maschine auf dem MacBook; `allan` ist dafür veröffentlicht; ein Runner und weitere Pakete stehen noch aus)
- [ ] riscv64 — sobald es echte Hardware zum Testen gibt
- [x] Die C-Bibliothek in der Plattform. glibc, musl (Alpine, Void musl) und bionic (Android) sind verschiedene ABIs, und Void Linux liefert sowohl glibc als auch musl: Heute bedeutet `linux-x86_64` glibc, sodass ein musl-Rechner einen Build nähme, der nicht einmal startet (geprüft: der dynamische Build antwortet auf Void musl mit „not found“). Entscheidung: Die Plattform lernt die C-Bibliothek, erkannt zur Laufzeit (der Interpreter von `/bin/sh`), nicht beim Bauen. Builds: `linux-x86_64` (glibc, wie bisher), `linux-x86_64-musl` und `linux-x86_64-static` für einen vollständig statischen Build, der auf jedem Linux läuft. Der Client nimmt zuerst seine genaue libc und danach den statischen Build; ein musl-Rechner nimmt nie einen glibc-Build. Vollständig statische Pakete (Go, Rust, offizielle statische Builds wie ripgrep, jq, btop) werden einmal als `static` veröffentlicht. WARP selbst wird als statische Binärdatei ausgeliefert (`tools/build-static.sh`: läuft auf Gentoo mit glibc und auf Void musl gleichermaßen). Die Umfrage lernt die libc ebenfalls (Berichtsschema 3, musl ist eine eigene Plattform). Erledigt in 0.4.6: Der Client erkennt die libc, `WARP_LIBC` überschreibt sie, `warp platform --all` listet die Kandidaten; die statischen k1os-Pakete tragen `_static`, und die oberste Ebene des Index fällt für ältere Clients darauf zurück
- [ ] CI-Runner für die anderen Plattformen (jeder wird ein eigener Workflow, damit der CI-Block auf der Seite ihn zeigt): natives macOS (auf dem MacBook), linux/aarch64 (die OrbStack-Maschine auf dem MacBook ist bereit), Termux (noch keiner; wird von Hand gestartet, wenn das Telefon zu Hause am Ladegerät ist, damit es unterwegs nicht den Akku leert)

## Versionen nebeneinander (Idee des Benutzers, 2026-10-01)
Heute liegen schon mehrere Versionen im Speicher (`store/<Name>-<hash12>`), aber es gibt nur einen `prev`-Link: Das Rollback wechselt zwischen zwei Versionen.
- [x] Beliebige Version installieren: `warp install name@1.2`, `warp versions name` (installiert und verfügbar); der Index führt alte Versionen (`name@version`)
- [x] `warp switch name <Version>` auf jede installierte Version; Rollback geht durch einen Aktivierungsverlauf zurück, nicht durch einen einzelnen Platz
- [x] `warp pin name [Version]` / `unpin`: `warp upgrade` lässt ein festgelegtes Paket in Ruhe (für ein Programm, das eine Version nicht neuer als X oder nicht älter braucht)
- [x] `warp run name@1.2 -- args`: eine bestimmte Version starten, ohne die aktive umzuschalten
- [x] `warp gc`: Versionen entfernen, die weder aktiv noch festgelegt noch im jüngsten Verlauf sind (der Platz ist begrenzt)
- [ ] Verschiedene abhängige Pakete nutzen gleichzeitig verschiedene Versionen einer Abhängigkeit (das Closure-Modell von Nix): Pakete müssen ihre Abhängigkeiten über den Speicherpfad finden; nach dem Abhängigkeitsauflöser

## macOS (nativ)
Auf Apple Silicon ohne Homebrew geprüft: WARP baut allein mit den Command Line Tools, linkt nur die System-`libcurl` und `libSystem`, weiß, dass es `macos-aarch64` ist, prüft einen echten signierten Index und lehnt ein Paket ohne macOS-Build mit einer klaren Meldung ab.
- [x] Kein OpenSSL (eigenes SHA-256, mitgeliefertes Ed25519), sodass zum Bauen nur die Command Line Tools nötig sind
- [x] Das CA-Bündel wird pro System gesucht (macOS hält es in `/etc/ssl/cert.pem`)
- [x] Ein Präfix im Besitz des Benutzers ohne Root (`~/.warp` oder `/opt/warp`), `warp shellenv` für das Shell-Profil — erledigt: `/opt/warp` unter macOS, `$PREFIX` in Termux, `warp shellenv` gibt die PATH-Zeile aus
- [x] Ein Installer: ein Befehl, der erklärt, was er tut, auf Enter wartet, die fertige `macos-aarch64`-Binärdatei holt und den Fingerabdruck des festgelegten Schlüssels zum Abgleich mit der Seite zeigt; später ein `.pkg` für verwaltete Installationen — erledigt: `tools/install.sh` (auch für Linux und Termux); den signierten Index holt warp danach selbst
- [x] Systemangaben für die Umfrage unter macOS (`sysctl`: Betriebssystemversion, CPU, Kerne, Speicher) — erledigt
- [x] Die ersten für `macos-aarch64` gebauten Pakete (ripgrep und jq aus den offiziellen Releases; btop veröffentlicht keinen macOS-Build), veröffentlicht mit einem `builds`-Eintrag — erledigt
- [x] Portable Tests (kein GNU-spezifisches `tar -I`, `timeout`, `stat -c`): Sie laufen unter nativem macOS, und der macOS-Job führt sie aus (der Docker-Ende-zu-Ende-Test bleibt unter Linux)
- [ ] Zwei CI-Runner auf dem MacBook: ein nativer macOS-Runner und der linux/aarch64-Runner (die OrbStack-Maschine, schon vorbereitet)
- [ ] Langfristig: Homebrew auf diesem Rechner durch WARP ersetzen (das Repository wird zum Basis-Repository)

## Termux (Android)
WARP wurde dort versehentlich gestartet (0.4.1 aus dem Quelltext gebaut und lief); ein SSH-Zugang zum Telefon besteht, es lässt sich also direkt testen.
- [x] Präfixbewusste Pfade: Speicher, Binärdateien und temporäre Dateien unter `$PREFIX` (kein `/var/lib`, kein `/usr/local/bin`, kein Root); das CA-Bündel unter `$PREFIX/etc/tls/cert.pem` (wird seit 0.4.5 gefunden) — erledigt: `$PREFIX/var/lib/warp`, `$PREFIX/bin`, `$PREFIX/tmp`
- [x] Installer für Termux (ohne `pkg`, fertige `android-aarch64`-Binärdatei) — erledigt: derselbe `tools/install.sh`
- [ ] Die ersten `android-aarch64`-Pakete (statische Go- und Rust-Binärdateien) — begonnen: `allan` 0.3.4 für `android-aarch64` (nativ in Termux mit Go gebaut, CGO aus, vom selben Tag wie die anderen Plattformen; die Go-Toolchain für Termux wird ein Paket, sobald warp `pkg` ersetzt)
- [ ] Ein CI-Runner auf dem Telefon, der von Hand gestartet wird, wenn es zu Hause am Ladegerät ist, damit er unterwegs nicht den Akku leert

## Dokumentation auf Deutsch
- [x] `README.de.md`, `ROADMAP.de.md` und `docs/NODES.de.md` neben den englischen und russischen
- [x] Die Seite zeigt die deutsche Dokumentation auch auf der Projektseite

## Echtzeit auf der Seite
- [x] Die Tracker-Karten (Admin-Seite und öffentliche Projektkarte) werden über einen WebSocket aktualisiert statt per Abfrage: Der Tracker schickt einen neuen Schnappschuss, wenn ein Knoten sich anmeldet oder berichtet (die Abfrage bleibt als Rückfall, solange der Socket nicht läuft)

## Paketautomatisierung
- [ ] Ein Rezept pro Paket in einem eigenen Repository: woher die Version kommt, wie sie zu prüfen ist (Prüfsumme oder Signatur des Upstreams), wie sie gebaut wird, wo die Lizenz steht
- [ ] Ein zeitgesteuerter Versionswächter (wie nvchecker / Anitya), der Rezepte mit dem Index vergleicht
- [ ] Eine neue Version wird in der CI gebaut, in einem Container installiert und als PR mit den Upstream-Änderungen vorgeschlagen
- [ ] Schwachstellen aus OSV.dev: Ein PR für eine Version mit bekannter CVE wird als dringend markiert
- [ ] Das Signieren des Index bleibt beim Menschen: Automatisierung kann nie von sich aus ein Paket ausliefern

## Als Nächstes
- [ ] Abhängigkeitsauflösung auf dem signierten Index
- [ ] Paketlizenzen: vor dem Installieren (aus dem Index) und danach (aus dem installierten Paket) lesen, z. B. `warp license <Paket>`
- [ ] Optionale Paketkomponenten (z. B. CUDA-, ROCm-, Vulkan-Backends): ein Paket, der Client installiert nur die zur Hardware passenden, `--with` / `--without` zur Wahl von Hand
- [ ] Was ist neu in einer Version: Release-Hinweise im signierten Index, angezeigt von `warp upgrade` und `warp info` vor dem Aktualisieren und von `warp changelog <Paket>` danach
- [ ] Funktionierende K1OS-Index-Spiegel auf GitLab und GitVerse
- [ ] zstd-Kompression
- [ ] Deklarative Systembeschreibung (`system.yaml`)
- [ ] Bauen und Betrieb in Termux
- [ ] K1K-Dienste als WARP-Pakete nach `/SVC` ausliefern

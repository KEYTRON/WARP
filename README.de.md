# WARP

[English version](README.md) · [Русская версия](README.ru.md)

WARP ist ein kleiner, in C geschriebener Paketmanager für K1OS und eine Handvoll
weiterer Distributionen. Er lädt signierte Paketarchive herunter, prüft sie und
verwaltet unter `/var/lib/warp` einen versionierten lokalen Speicher mit sofortigem Rollback.

![Tests](https://github.com/KEYTRON/WARP/actions/workflows/warp-tests.yml/badge.svg)
![K1OS](https://github.com/KEYTRON/WARP/actions/workflows/warp-k1os.yml/badge.svg)
![Alpine](https://github.com/KEYTRON/WARP/actions/workflows/warp-alpine.yml/badge.svg) ![AlmaLinux](https://github.com/KEYTRON/WARP/actions/workflows/warp-almalinux.yml/badge.svg) ![Arch](https://github.com/KEYTRON/WARP/actions/workflows/warp-arch.yml/badge.svg) ![Artix](https://github.com/KEYTRON/WARP/actions/workflows/warp-artix.yml/badge.svg) ![Devuan](https://github.com/KEYTRON/WARP/actions/workflows/warp-devuan.yml/badge.svg) ![Fedora](https://github.com/KEYTRON/WARP/actions/workflows/warp-fedora.yml/badge.svg) ![Ubuntu](https://github.com/KEYTRON/WARP/actions/workflows/warp-ubuntu.yml/badge.svg) ![Void glibc](https://github.com/KEYTRON/WARP/actions/workflows/warp-void.yml/badge.svg) ![Void Musl](https://github.com/KEYTRON/WARP/actions/workflows/warp-void-musl.yml/badge.svg)

[Alle WARP-Workflow-Läufe ansehen](https://github.com/KEYTRON/WARP/actions) — jeder
Workflow läuft auf dem selbst betriebenen Runner des K1-Labors.

## Vertrauensmodell — „jeder ist ein Feind“

WARP geht davon aus, dass jede Station im Netz feindlich ist, und vertraut genau
einer Sache: einem Ed25519-Schlüssel, den Sie lokal festgelegt haben.

- Ein Repository besteht aus einer Basis-URL (plus optionalen Spiegeln) und einem
  **öffentlichen Schlüssel**. Der Schlüssel wird beim Hinzufügen des Repositorys
  festgelegt; er wandert nie zusammen mit den Daten.
- Jeder Spiegel liefert `index.json` und die abgetrennte Signatur `index.json.sig`
  nebeneinander aus. WARP lädt beide **vom selben Spiegel** und prüft die Signatur
  mit dem festgelegten Schlüssel, *bevor* der Index zwischengespeichert oder gelesen wird.
  Keine gültige Signatur — kein Index; einen unsignierten Modus gibt es nicht.
- Der signierte Index ist die geschlossene Welt: Paketname, Version, URL, Größe und
  `sha256` jedes Archivs und jedes Deltas. Heruntergeladene Bytes werden gegen den
  Index geprüft, bevor irgendetwas installiert wird. Spiegel, Torrents und P2P-Peers
  sind nur Transport — sie können kein Paket einschleusen und keinen Hash ändern.
- Auch Abhängigkeiten stehen im signierten Index (siehe unten); das Manifest im
  Archiv dient der Information und muss mit dem Index übereinstimmen.

## Bauen

```bash
make            # oder ./build.sh build
```

WARP braucht einen C-Compiler, `make` und die libcurl-Header, sonst nichts: Es hat
keine OpenSSL-Abhängigkeit (SHA-256 ist eigener Code, Ed25519 ist das mitgelieferte
Monocypher in `src/vendor/`). Unter macOS genügen die Command Line Tools.
`tools/build-static.sh` baut eine vollständig statische Binärdatei (musl, Docker nötig),
die auf glibc- und musl-Systemen gleichermaßen läuft.

## Installieren

```bash
sudo make install       # oder sudo ./build.sh install
```

Die Binärdatei landet in `/usr/local/bin/warp`; mit `PREFIX` lässt sich ein anderer Ort wählen.

## Befehle

| Befehl | Was er tut |
|---|---|
| `warp update` | Den Index jedes aktivierten Repositorys aktualisieren (mit Signaturprüfung) |
| `warp search <Suchbegriff>` | Verfügbare Pakete durchsuchen |
| `warp install <Paket>` | Ein Paket installieren (`repo/paket`, um ein Repository ausdrücklich zu wählen) |
| `warp upgrade [Paket...]` | Installierte Pakete aktualisieren, per Delta, wenn eines veröffentlicht ist |
| `warp remove <Paket>` | Ein Paket entfernen |
| `warp rollback <Paket>` | Eine Version zurück durch den Aktivierungsverlauf (wiederholen für weiter zurück) |
| `warp versions` / `switch` / `pin` / `unpin` / `run` / `gc` | Versionen nebeneinander, siehe unten |
| `warp list` / `warp info <Paket>` | Installierte Pakete / Details |
| `warp repo list\|add\|remove\|enable\|disable` | Repositorys verwalten |
| `warp delta <alt> <neu> <aus>` | Ein binäres Delta zwischen zwei Archiven erzeugen |
| `warp delta-apply <alt> <delta> <aus>` | Das neue Archiv aus dem alten und einem Delta wiederherstellen |
| `warp keygen [priv pub]` | Ein Ed25519-Signaturschlüsselpaar erzeugen |
| `warp sign <Datei> [priv]` | Eine abgetrennte Base64-Signatur `<Datei>.sig` schreiben |
| `warp verify <Datei> --pubkey <hex>` | `<Datei>.sig` (Ed25519) gegen einen öffentlichen Schlüssel prüfen |
| `warp pubkey <Datei-des-privaten-Schlüssels>` | Den zu einem privaten Schlüssel gehörenden öffentlichen Schlüssel ausgeben |
| `warp sha256 <Datei>...` | SHA-256-Summen ausgeben (wie `sha256sum`) |
| `warp pack <Verzeichnis>` | Ein `.warp`-Archiv aus einem Verzeichnis erstellen |
| `warp seed` | Den Knoten betreiben: installierte Pakete an Peers verteilen (P2P-Transport) |
| `warp volunteer` | Derselbe Knoten, dazu selten verteilte Pakete im Rahmen Ihrer Grenzen zwischenspeichern |
| `warp stats` | Dieser Knoten und das Netz: gesendete Daten, Pakete, Knoten online, Verkehr |

## Verteilen, Freiwilligenmodus und Statistik

Es gibt einen einzigen Knotenprozess (`warp seed`, Dienst `warp-seed`). Der
Freiwilligenmodus ist eine Einstellung dieses Knotens und kein zweiter Dienst; die
beiden können sich also nie um Port 7777 streiten.

```bash
warp volunteer --setup               # interaktiv: Platz, Paketzahl, SD-Karten-Modus, Monatslimit
warp volunteer --quota 10.46G        # Platz für den Cache: 10G, 10.46G, 500M oder all
warp volunteer --packages 46         # höchstens 46 Pakete (oder all)
warp volunteer --reserve 2G          # immer 2 GiB des Datenträgers frei lassen (Standard 1G)
warp volunteer --disable             # zurück zum einfachen Verteilen; ein laufender Knoten übernimmt es sofort
```

Mit Grenzen füllt der Knoten den Cache zuerst mit den am seltensten verteilten Paketen.
Mit `--quota all` nimmt er alles. Ein laufender Knoten liest seine Einstellungen neu ein,
wenn `warp volunteer ...` sie ändert (SIGHUP), und sucht alle sechs Stunden erneut nach
selten verteilten Paketen.

`warp stats` zeigt, was dieser Knoten getan hat (gesendete Bytes und Pakete, Cache,
Monatsverkehr) und den Zustand des Netzes (Knoten online, Pakete, bewegte Daten).
`warp --help` endet mit einer Zusammenfassung in einer Zeile, die nur aus lokalen Dateien stammt.

**Anonyme Statistik ist aus, bis Sie Ja sagen.** Das erste interaktive
`warp seed` / `warp volunteer` zeigt genau, was gesendet würde, und fragt
`[y/N]`; Enter heißt Nein. `warp stats --what` zeigt es erneut, `warp stats
--consent` / `--no-stats` ändern die Antwort, `--reset-id` erzeugt eine neue
zufällige Knoten-ID. Der Bericht enthält eine zufällige Knoten-ID, die warp-Version,
das Betriebssystem und die Architektur sowie ein grobes Hardwareprofil wie bei einer
Hardware-Umfrage (Kernel nur als major.minor, Distribution und Version, CPU-Modell und
Kernzahl, Arbeitsspeicher auf eine Standardgröße gerundet); all das speist die öffentliche
Plattform-Umfrage: nur Prozentwerte, ein Eintrag pro Knoten und Monat. Außerdem steht darin,
ob der Freiwilligenmodus an ist, wie viele Bytes und Pakete gesendet wurden und wie viele
Pakete der Knoten verteilen kann — keine Dateinamen, keine Pfade, keine Paketinhalte.
Das Verteilen selbst teilt dem Tracker weiterhin Ihre Adresse, den Port und die Namen der
verteilten Pakete mit (Peers brauchen das, um Sie zu finden); das ist unabhängig von der
Statistik. Die Netzsummen sind die Summe dessen, was zustimmende Knoten melden, und werden
nicht überprüft.

Wenn eine neue Version dem Bericht Felder hinzufügt, deckt die frühere Antwort sie nicht ab: Der Knoten sendet nichts, bis Sie den neuen Bericht gesehen haben (`warp stats --consent`).

## Versionen nebeneinander

Jede installierte Version liegt im Speicher als `store/<Name>-<hash12>`; eine davon
ist aktiv. Mehrere können nebeneinander liegen, damit ein Programm, das eine ältere
Version braucht (oder noch keine neuere), weiter funktioniert.

```bash
warp versions tool            # im Speicher installiert und im Index veröffentlicht
warp install tool@1.2         # dieses Release neben dem aktuellen; es wird aktiv und festgelegt (pinned)
warp switch tool 2.0          # jede installierte Version, ohne Download
warp pin tool [1.2]           # `warp upgrade` lässt es in Ruhe (mit einer Version wird auch dorthin gewechselt)
warp unpin tool
warp rollback tool            # ein Schritt zurück durch den Aktivierungsverlauf; wiederholen, um weiter zu gehen
warp run tool@1.1 -- args     # eine gespeicherte Version starten, ohne umzuschalten (--bin <Name> wählt das Programm)
warp gc [--keep N] [--dry-run]   # Versionen entfernen, die weder aktiv noch festgelegt noch unter den letzten N Aktivierungen sind
```

Ein älteres Release wird bewusst gewählt, deshalb legt `warp install tool@1.2` es fest:
Sonst würde das nächste `warp upgrade` es rückgängig machen. Eine Festlegung folgt einem
ausdrücklichen `switch` oder `rollback`. `warp rollback` geht durch jede Aktivierung zurück,
nicht nur zwischen den letzten beiden. Alte Releases sind installierbar, wenn das Repository
sie veröffentlicht: `tools/make-index.py --keep-old-versions` fügt für jedes ältere Release
einen Eintrag `name@version` neben dem einfachen `name` (dem neuesten) hinzu; Clients, die
älter sind, ignorieren die zusätzlichen Einträge. Noch nicht abgedeckt: verschiedene Programme,
die gleichzeitig verschiedene Versionen derselben *Abhängigkeit* nutzen (Closures im Stil von Nix);
siehe Roadmap.

## Plattformen: nur fertige Pakete

WARP lädt nie Quelltext herunter, um ihn zu kompilieren. Jedes Paket wird in der CI für
jede unterstützte Plattform gebaut, und der signierte Index führt pro Plattform einen
**Build** auf, geschrieben `<os>-<arch>`: `linux-x86_64`, `linux-aarch64`,
`android-aarch64` (Termux), `macos-aarch64`. Der Client installiert nur den Build für
sein eigenes Betriebssystem und seine CPU (`warp platform` zeigt ihn an). Hat ein Paket
keinen Build für Ihre Plattform, sagt `warp install` das und nennt die Plattformen, für
die es einen hat; es greift nicht auf eine Binärdatei für ein anderes System zurück.
Architekturunabhängige Pakete (Skripte, Daten) werden einmal als `any` veröffentlicht.

Archive heißen unter Linux `name-version-<arch>.warp` (x86_64, aarch64) und sonst
`name-version-<os>_<arch>.warp` (`android_aarch64`); `noarch` heißt: jede Plattform.
`tools/make-index.py` gruppiert sie nach Plattform.

Paketbeschreibungen kommen in der Sprache des Benutzers, wenn der Index sie enthält: Ein
Eintrag kann neben der englischen `description` eine Map `descriptions` tragen
(`{"ru": "...", "de": "..."}`), und `warp search` / `warp info` wählen den Text wie gettext
(`WARP_LANG`, dann die Locale, wobei `C` Englisch bedeutet, dann `LANGUAGE`).
`tools/make-index.py` liest die Übersetzungen aus einer `descriptions.json` im
Repository-Verzeichnis.

## Repositorys

Das eingebaute Repository `k1os` (Spiegel auf GitHub, GitLab und GitVerse, Schlüssel in
die Binärdatei einkompiliert) ist immer vorhanden und lässt sich nicht entfernen. Weitere
Repositorys stehen in `/var/lib/warp/repos.json`, jedes mit eigenem Cache unter
`/var/lib/warp/repos/<Name>/`.

```bash
warp repo add lab https://mirror.example/lab --pubkey <hex64> [--mirror <url>]...
warp repo add lab https://mirror.example/lab --pubkey-file lab.pub
warp repo list
warp repo disable lab      # konfiguriert lassen, aber überspringen
warp repo remove lab
```

Der öffentliche Schlüssel ist beim `add` **Pflicht**: WARP kennt keinen Begriff von einem
nicht vertrauenswürdigen Repository. Repository-Namen entsprechen `[a-z0-9_-]`; veröffentlichen
zwei aktivierte Repositorys denselben Paketnamen, gewinnt das zuerst aufgeführte, und
`warp install anderes/paket` wählt ein bestimmtes.

`warp update` beendet sich mit Code 1, wenn mindestens ein aktiviertes Repository nicht
aktualisiert werden konnte (die Signatur stimmte nicht oder kein Spiegel antwortete). Pakete
aus den anderen Repositorys bleiben verfügbar.

### Das erste Repository: `keytron`

Das erste öffentliche Repository neben dem eingebauten `k1os` ist `keytron` auf
[keytron-prime.org](https://keytron-prime.org). Es wird mit einem Befehl hinzugefügt:

```bash
sudo warp repo add keytron https://keytron-prime.org/packages/keytron --pubkey 53d0a36597b812873cdaa42b11b08592ac3f0998998ccb7e59d6833640f9d883
sudo warp update
```

Den Schlüssel können Sie mit der veröffentlichten
[`pubkey.hex`](https://keytron-prime.org/packages/keytron/pubkey.hex) abgleichen.
Der Signaturschlüssel dieses Repositorys liegt nicht auf dem ausliefernden Rechner.

### Ein eigenes Repository veröffentlichen

Jeder statische HTTP-Host genügt. Legen Sie die `.warp`-Archive in ein Verzeichnis und führen Sie aus

```bash
warp keygen repo.priv repo.pub                       # einmalig
tools/make-index.py <Verzeichnis> --base-url https://mirror.example/lab --key repo.priv
```

`make-index.py` führt die tatsächlich vorhandenen Archive auf (echte `sha256` und Größe —
Einträge, deren Archiv fehlt, werden verworfen, damit der Index nie etwas anbietet, was der
Spiegel nicht liefern kann), erzeugt Deltas aus älteren Archiven im Verzeichnis und signiert
das Ergebnis mit `warp sign`. Laden Sie das Verzeichnis unverändert hoch; geben Sie `repo.pub`
an Ihre Benutzer weiter.

Index und Archive des eingebauten `k1os` liegen im Branch `packages` von
`KEYTRON/K1OS` (Git LFS für die `.warp`-Dateien) und werden ausgeliefert von:

1. `https://github.com/KEYTRON/K1OS/raw/packages`
2. `https://gitlab.com/KEYTRON/K1OS/-/raw/packages`
3. `https://gitverse.ru/keytron46/K1OS/raw/branch/packages`

## Aktualisierungen und Delta-Updates

`warp upgrade` aktualisiert die Indizes und vergleicht die `sha256` jedes installierten
Archivs mit dem Index. Führt der Index ein Delta, dessen `from_sha256` zum Archiv passt,
das Sie bereits im Speicher haben, lädt WARP nur das Delta, prüft dessen eigenen Hash,
baut das neue Archiv lokal mit `delta-apply` wieder auf und prüft das Ergebnis dann gegen
die `sha256` des vollständigen Archivs aus dem Index — ein Delta kann also nie Bytes liefern,
die der Index nicht abgesegnet hat. Passt kein Delta oder liefert der Spiegel es nicht aus,
fällt WARP auf das vollständige Archiv zurück. Die vorherige Version bleibt für `rollback`
im Speicher.

Deltas sind inhaltsbasiert: Archive werden mit einer rollenden Gear-Hash-Grenze in variable
Blöcke zerlegt (2–64 KiB, im Mittel ~16 KiB), sodass eine Einfügung am Dateianfang nichts
anderes verschiebt. Format `WARPDLT1`: Kopfzeile mit alter und neuer Größe + `sha256`, danach
ein Strom von Operationen `COPY(Offset, Länge)` / `LIT(Bytes)`. Damit sich Deltas lohnen,
müssen die Archive reproduzierbar komprimiert sein — packen Sie sie mit `gzip -n --rsyncable`
(wie es `tools/make-package.sh` und `make-index.py` tun); ein einfaches `gzip` verwürfelt
den ganzen Strom nach dem ersten geänderten Byte.

Typische Zahlen aus der Testsuite: ein 2-MiB-Archiv mit 50 KiB Änderung → ~100 KiB Delta.
Deltas lohnen sich nur bei Archiven, die im Verhältnis zur Änderung groß sind — ein
40-KiB-Archiv, dessen Code größtenteils neu geschrieben wurde (warp 0.3.3 → 0.4.0), spart
nichts, deshalb führt `make-index.py` keine Deltas auf, die 90 % oder mehr des vollständigen
Archivs ausmachen.

## Abhängigkeiten

Abhängigkeiten stehen im **signierten Index**, nicht im Archiv:

```json
"deps": [ {"name": "openssl", "version": "3.3.2"}, {"name": "libfoo", "version": "1.2", "repo": "lab"} ]
```

Jede Abhängigkeit ist durch Name und genaue Version (also genaue `sha256`) im selben
Repository festgelegt, es sei denn, `repo` nennt ausdrücklich ein anderes — ein Repository
kann keine Pakete aus einem Repository einziehen, das es nicht nennt, und ein Paket kann
nicht durch eine „ähnliche“ Version aus einer nicht vertrauenswürdigen Quelle erfüllt werden.
Das Manifest im Archiv darf die Liste für Menschen wiederholen; widerspricht es dem Index,
wird die Installation verweigert. Die Auflösung über diese geschlossene Welt ist der nächste
Schritt der Roadmap; heute liest und zeigt der Client `deps`.

## Indexformat

```json
{
  "timestamp": "2026-09-19",
  "packages": {
    "warp": {
      "version": "0.4.0",
      "description": "...",
      "sha256": "…", "size": 6104096,
      "url": "https://…/warp-0.4.0-x86_64.warp",
      "deltas": [
        {"from_version": "0.3.3", "from_sha256": "…", "url": "https://…/warp-0.3.3-to-0.4.0-x86_64.warpdelta", "sha256": "…", "size": 180439}
      ],
      "deps": [],
      "variants": [ {"kind": "torrent", "url": "magnet:?…", "sha256": "…", "priority": 10} ]
    }
  }
}
```

Ein Paket, das für mehr als `linux-x86_64` gebaut ist, trägt eine Map `builds`, nach
Plattform geordnet. Jeder Build hat seine eigene `version`, `sha256`, `size`, `url` und
`deltas` (ein Build darf `version` weglassen und die des Eintrags verwenden); die oberste
Ebene beschreibt dann weiterhin den Build `linux-x86_64`, damit Clients, die `builds` noch
nicht kennen, weiter funktionieren. Ein nur für `linux-x86_64` veröffentlichtes Paket behält
das einfache Format von oben.

```json
"tool": {
  "description": "...",
  "version": "1.0", "sha256": "…", "size": 1234, "url": "https://…/tool-1.0-x86_64.warp",
  "builds": {
    "linux-x86_64":    {"version": "1.0", "sha256": "…", "size": 1234, "url": "https://…/tool-1.0-x86_64.warp"},
    "linux-aarch64":   {"version": "1.0", "sha256": "…", "size": 1180, "url": "https://…/tool-1.0-aarch64.warp"},
    "android-aarch64": {"version": "1.0", "sha256": "…", "size": 1190, "url": "https://…/tool-1.0-android_aarch64.warp"}
  }
}
```

`variants` ist optional: `kind` ist `direct`, `torrent`, `magnet`, `p2p`, `http`,
`https` oder `file`; höhere `priority` gewinnt innerhalb einer Transportklasse. Ohne
sie werden `url`/`sha256` der obersten Ebene verwendet. `index.json.sig` ist die
Base64-Ed25519-Signatur der rohen Bytes von `index.json` (`warp sign index.json`), kein Feld
innerhalb des JSON — eine Signatur kann kein Dokument abdecken, das sie selbst enthält.

Das optionale Feld `peer_list_url` ist der P2P-Tracker dieses Repositorys. Ohne es werden die
Pakete des Repositorys nur von seinen Spiegeln geholt, und Installationen werden nirgends
gemeldet. Der einkompilierte Tracker `keytron-prime.org/warp` wird nur für das eingebaute
`k1os` verwendet.

## Tests

```bash
sh tests/delta-roundtrip.sh        # Delta bauen/anwenden, falsche Basis und Abschneiden werden abgelehnt
sh tests/crypto.sh                 # SHA-256 und Ed25519: RFC/NIST-Vektoren, Fälschungen, OpenSSL als Orakel
sh tests/versions.sh               # Versionen nebeneinander: beliebige installieren, wechseln, festlegen, Rollback-Kette, run, gc
sh tests/multi-platform.sh         # ein Build pro Betriebssystem/CPU im Index; der Client nimmt nur seinen eigenen
sh tests/node-stats.sh             # Knoten: Freiwilligengrenzen, ein Prozess, Zähler, Statistik nur mit Zustimmung
sh tests/e2e-delta-upgrade.sh      # eigenes Repo über HTTP, install → Upgrade per Delta → Rollback, im K1OS-Container
```

Der letzte Test braucht Docker und `ghcr.io/keytron/k1os:latest`; alle laufen in der
CI (`warp-tests.yml`).

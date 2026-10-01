# WARP-Repositorys und -Knoten

Sprache: [English](NODES.md) | [Русский](NODES.ru.md) | Deutsch

**Status: Entwurf, nicht umgesetzt.** Namen von Befehlen und Feldern sind vorläufig.

## Warum

- **Repository-Modus** — jeder, ob Person oder Firma, kann mit ein oder zwei Befehlen ein eigenes Paket-Repository betreiben.
- **Knoten-Modus** — Knoten übernehmen die Auslieferung aller Pakete eines Repositorys oder eines Teils davon. Das entlastet den Repository-Host und ermöglicht horizontale Skalierung.
- **Repositorys hinzufügen** gibt es im Client schon (`warp repo add … --pubkey`) und bleibt der wichtigste Weg, einem Repository zu vertrauen.

## Das Grundprinzip

Die Paketwelt ist geschlossen: Ein Paket existiert nur, wenn das Repository seinen Namen, seine Version und seine sha256 signiert hat. Knoten sind Transport. Sie können kein Paket hinzufügen, nicht verändern und keine andere Version unterschieben: Der Client prüft jedes heruntergeladene Byte gegen den signierten Index.

Zum Signieren genügt die sha256, nicht die Datei selbst. Ein Paket darf also nur auf Knoten liegen (wenn dem Host Platz oder Bandbreite ausgehen) und trotzdem vom Repository signiert sein.

## Schlüsselrollen

Heute macht ein einziger Schlüssel alles. Damit der Diebstahl eines Schlüssels niemandem erlaubt, Schadsoftware auszuliefern, werden die Rollen getrennt:

| Schlüssel | Wo er liegt | Was er signiert | Wenn er gestohlen wird |
|---|---|---|---|
| Root | offline | `root.json`: welche Rollenschlüssel gültig sind und bis wann | alles — deshalb bleibt er offline und wird selten benutzt |
| Indexschlüssel | nicht auf dem ausliefernden Host | `index.json`: Pakete, sha256, Deltas | Schadsoftware kann signiert werden — Ersatz durch den Root-Schlüssel |
| Online-Schlüssel | auf dem Server | `nodes.json` und `timestamp.json`, kurzlebig | nur Knotenliste und Aktualität, innerhalb ihrer Lebensdauer; Pakete lassen sich nicht signieren |

Der vom Client festgelegte Schlüssel (heutiges `--pubkey`) wird zum Root-Schlüssel. Rollenschlüssel werden mit einer neuen, vom Root-Schlüssel signierten `root.json` gewechselt. Der Root-Schlüssel selbst wird mit einer neuen `root.json` gewechselt, die vom alten und vom neuen Root-Schlüssel signiert ist.

## Dateien des Repositorys

| Datei | Signiert von | Lebensdauer | Inhalt |
|---|---|---|---|
| `root.json` | Root-Schlüssel | lang (z. B. ein Jahr) | Rollenschlüssel und ihre Lebensdauer |
| `index.json` | Indexschlüssel | mittel | Pakete, sha256, Größen, Deltas, Fundorte; eine steigende `version` |
| `nodes.json` | Online-Schlüssel | kurz (z. B. ein Tag) | Knotenzertifikate |
| `timestamp.json` | Online-Schlüssel | sehr kurz (z. B. eine Stunde) | Version und sha256 der aktuellen `index.json` und `nodes.json` |

## Schutz vor Rollback — der erste Schritt

Heute liest der Client den `timestamp` des Index, vergleicht ihn aber nie mit dem, was er schon gesehen hat. Wer den Index ausliefert, kann einen alten, aber echt signierten Index herausgeben und den Client auf Versionen mit bekannten Schwachstellen festhalten. Deshalb vor den Knoten:

- jede signierte Datei trägt eine steigende `version` und ein Ablaufdatum `expires`;
- der Client merkt sich die zuletzt gesehene Version und lehnt eine Datei mit niedrigerer Version oder eine abgelaufene mit einer klaren Fehlermeldung ab.

Ohne das wird der Knoten-Modus nicht eingeschaltet.

## Knotenzertifikat

Ein Eintrag in `nodes.json`:

```json
{
  "node_id": "fra-1",
  "pubkey": "<der Ed25519-Schlüssel des Knotens>",
  "urls": ["https://mirror.example/warp"],
  "scope": {"packages": ["rust", "go"]},
  "issued": "2026-09-23T12:00:00Z",
  "expires": "2026-09-24T12:00:00Z"
}
```

`scope` ist: alle Pakete oder ein Teil davon, als Liste von Namen oder als Hash-Bereich für gleichmäßige Aufteilung.

## Beitritt — beide Seiten stimmen zu

1. **Der Knotenbetreiber** erzeugt einen Knotenschlüssel und schickt dem Repository eine Anfrage: den Schlüssel, Adressen und den Anteil, den er zu speichern bereit ist. Die Anfrage ist mit dem Knotenschlüssel signiert und beweist, dass der Knoten ihn besitzt.
   `warp node init` → `warp node request <Repository-URL>`
2. **Der Repository-Eigentümer** sieht die Anfrage und genehmigt sie mit einem Anteil und einer Lebensdauer.
   `warp repo nodes pending` → `warp repo nodes approve <id> --scope … --days N`
3. **Synchronisation.** Der Knoten lädt die Pakete seines Anteils herunter und prüft jedes gegen die sha256 im signierten Index. Bei einer Abweichung liefert er die Datei nicht aus.
4. **Bereitschaftsprüfung.** Das Repository fragt den Knoten nach zufälligen Blöcken von Paketen seines Anteils. Erst nach der Antwort kommt der Knoten in `nodes.json`.
5. **Erneuerung.** Der Online-Schlüssel baut `nodes.json` regelmäßig mit neuer Lebensdauer neu auf, aber nur für Knoten, die die Verfügbarkeitsprüfung bestehen. Ein Knoten, der verschwindet, fällt bei der nächsten Erneuerung von selbst heraus.
6. **Widerruf.** Der Knoten wird aus `nodes.json` entfernt; Clients erfahren es mit der nächsten `timestamp.json`.

Standardmäßig kommen nur genehmigte Knoten in ein Repository. Offene Anmeldung kann später eine Option werden: Sie lässt sich leichter mit toten Adressen fluten. Das heutige `warp seed` / `warp volunteer` wird zu einem Sonderfall eines Knotens.

## Wie der Client herunterlädt

1. `warp update`: `timestamp.json` → `index.json` und `nodes.json`. Signaturen, Versionen und Lebensdauern werden geprüft.
2. Für ein Paket sind die Kandidaten das Repository selbst und jeder Knoten mit gültigem Zertifikat, dessen Anteil das Paket abdeckt.
3. Der Client probiert sie der Reihe nach (nach Latenz oder zufällig) und prüft die sha256. Bei Abweichung, Fehler oder langsamer Antwort wird der Knoten als schlecht markiert und der nächste genommen.

## Pakete von Knoten — Delegation, keine eigene Signatur

Ein Knoten **signiert keine** Pakete im Namen des Repositorys: Sonst liefert ein gekaperter Knoten Schadsoftware unter fremdem Vertrauen aus. Braucht ein Knoten eigene Pakete:

- **ein eigenes Repository** mit eigenem Schlüssel — der Benutzer fügt es ausdrücklich hinzu;
- **Namensraum-Delegation** — der Root-Schlüssel erlaubt dem Knotenschlüssel, nur Pakete mit einem Präfix zu signieren, z. B. `acme/*`. Der Client zeigt die Quelle, und der Knoten kann Basispakete (`warp`, `openssl` und so weiter) nie überschreiben.

## Risiken

- **Datenschutz:** Ein Knoten sieht die IP des Clients und was er herunterlädt. Firmen muss das ausdrücklich gesagt werden.
- **Nicht erreichbare oder langsame Knoten:** Der Client wechselt stillschweigend zu anderen Knoten oder zum Repository.
- **Ein gestohlener Online-Schlüssel:** kann die Knotenliste verderben oder Updates innerhalb der Lebensdauer von `nodes.json` / `timestamp.json` verzögern, aber kein Paket signieren.

## Etappen

1. Schutz vor Rollback.
2. Repository-Modus: `warp repo init` und `warp publish` statt eines externen Skripts, getrennte Schlüssel. Beim Veröffentlichen kommen Release-Hinweise (`--notes`) in den signierten Index, sodass ein Knoten oder Spiegel sie nicht austauschen kann.
3. Knoten-Modus: Zertifikate, Einvernehmen, Synchronisation, Prüfungen, Erneuerung, Client lädt von Knoten.
4. Namensraum-Delegation (falls nötig).

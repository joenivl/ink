#!/usr/bin/env python3
"""Zet het wedstrijdprogramma van een voetbalteam (KNVB/Sportlink) in een .ics-bestand.

Home Assistant leest dat bestand met de integratie Remote Calendar, zodat de
wedstrijden in een gewone kalender staan (en dus op het Ink-scherm). Zo hoef je
niet te betalen voor agenda-synchronisatie in de voetbal.nl-app.

Gebruik (vanuit shell_command in homeassistant/packages/voetbal.yaml):

    python3 voetbal.py --team O9-2 --uit /config/www/voetbal.ics

--team mag de teamnaam zijn ("O9-2", zoals op voetbal.nl maar zonder clubnaam)
of de Sportlink-teamcode (een getal). Meerdere teams: --team meerdere keren.

De gegevens komen standaard via de website van asv Dronten, die Sportlink met
haar eigen client_id aanroept. Heeft je club zelf een Sportlink client_id (staat
vaak in de broncode van de clubsite), geef die dan mee met --client-id; dan gaat
het rechtstreeks naar data.sportlink.com.

Gespeelde wedstrijden verdwijnen uit het programma van Sportlink. Het script
onthoudt ze daarom nog 5 weken (--cache), zodat ze in de week- en maandweergave
blijven staan. Alleen standaardbibliotheek, zodat het in de HA-container draait.
"""

import argparse
import json
import os
import sys
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

# Openbare sleutel van de clubsite (staat in hun JavaScript); geeft alleen
# toegang tot wat de website ook laat zien.
CLUB_PROXY = "https://ugnavudgvlplxrrwfsfs.supabase.co/functions/v1/sportlink"
CLUB_SLEUTEL = (
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InVnbmF2dWRndmxwbHhycndmc2Zz"
    "Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzQyNzU3MzQsImV4cCI6MjA4OTg1MTczNH0.er6cxF3l4eznCrGi4woOVuLSKRzoKYq0Dz8V6ftWUQ0"
)
SPORTLINK = "https://data.sportlink.com"
BEWAAR_GESPEELD = timedelta(weeks=5)


def ophalen(endpoint, params, client_id):
    if client_id:
        query = urllib.parse.urlencode({**params, "client_id": client_id})
        verzoek = urllib.request.Request(f"{SPORTLINK}/{endpoint}?{query}")
    else:
        verzoek = urllib.request.Request(
            CLUB_PROXY,
            data=json.dumps({"endpoint": endpoint, "params": params}).encode(),
            headers={
                "apikey": CLUB_SLEUTEL,
                "Authorization": f"Bearer {CLUB_SLEUTEL}",
                "Content-Type": "application/json",
            },
        )
    verzoek.add_header("User-Agent", "Ink-kalender (Home Assistant)")
    with urllib.request.urlopen(verzoek, timeout=30) as antwoord:
        data = json.load(antwoord)
    if not isinstance(data, list):
        raise RuntimeError(f"onverwacht antwoord van Sportlink: {str(data)[:200]}")
    return data


def teamcodes(teams, client_id):
    """Teamnamen ("O9-2") omzetten naar Sportlink-teamcodes."""
    codes, namen = [], [t for t in teams if not t.isdigit()]
    alle = ophalen("teams", {}, client_id) if namen else []
    for team in teams:
        if team.isdigit():
            codes.append(int(team))
            continue
        # Sinds 2025 heten JO9, JO11 enz. bij de KNVB O9, O11; de oude naam mag ook.
        if team.upper().startswith("JO"):
            team = team[1:]
        gevonden = {
            t["teamcode"] for t in alle
            if t.get("teamcode", -1) > 0 and t.get("teamnaam", "").lower().endswith(" " + team.lower())
        }
        if len(gevonden) != 1:
            bestaande = sorted({t["teamnaam"].split()[-1] for t in alle
                                if t.get("teamcode", -1) > 0 and t.get("teamnaam")})
            raise RuntimeError(
                f"team '{team}' {'niet gevonden' if not gevonden else 'is niet eenduidig'}; "
                f"teams van de club: {', '.join(bestaande)}"
            )
        codes.append(gevonden.pop())
    return codes


def esc(tekst):
    return (str(tekst).replace("\\", "\\\\").replace(";", "\\;")
            .replace(",", "\\,").replace("\n", "\\n"))


def vouw(regel):
    """RFC 5545: regels van hooguit 75 bytes, vervolg begint met een spatie."""
    data, delen = regel.encode(), []
    while len(data) > 75:
        knip = 75 if not delen else 74
        while (data[knip] & 0xC0) == 0x80:  # niet midden in een UTF-8-teken knippen
            knip -= 1
        delen.append(data[:knip].decode())
        data = data[knip:]
    delen.append(data.decode())
    return "\r\n ".join(delen)


def afspraak(sleutel, w, met_teamnaam, nu):
    eigen = w.get("teamnaam", "")
    thuis = w.get("thuisteam", "") == eigen
    tegen = w.get("uitteam") if thuis else w.get("thuisteam")
    titel = f"Thuis tegen {tegen}" if thuis else f"Uit bij {tegen}"
    if met_teamnaam:
        korte_naam = eigen.split()[-1] if eigen else ""
        titel = f"{korte_naam} {titel[0].lower()}{titel[1:]}"
    status = w.get("status", "")
    if status and status != "Te spelen":
        titel += f" ({status.lower()})"

    regels = []
    for label, veld in (("Verzamelen", "verzameltijd"), ("Vertrek", "vertrektijd"),
                        ("Rijders", "rijders"), ("Veld", "veld"),
                        ("Scheidsrechter", "scheidsrechter")):
        if w.get(veld):
            regels.append(f"{label}: {w[veld]}")
    kleedkamer = w.get("kleedkamerthuisteam" if thuis else "kleedkameruitteam")
    if kleedkamer:
        regels.append(f"Kleedkamer: {kleedkamer}")
    regels.append(w.get("wedstrijd", ""))
    if w.get("competitie"):
        regels.append(f"{w['competitie']}, {w.get('klassepoule', '')}".rstrip(", "))

    plaats = (w.get("plaats") or "").title()
    locatie = ", ".join(x for x in (w.get("accommodatie"), plaats) if x)

    start = datetime.strptime(w["wedstrijddatum"], "%Y-%m-%dT%H:%M:%S%z")
    if w.get("aanvangstijd") in (None, "", "00:00"):
        tijd = [f"DTSTART;VALUE=DATE:{start:%Y%m%d}",
                f"DTEND;VALUE=DATE:{start + timedelta(days=1):%Y%m%d}"]
    else:
        start = start.astimezone(timezone.utc)
        tijd = [f"DTSTART:{start:%Y%m%dT%H%M%SZ}",
                f"DTEND:{start + timedelta(minutes=90):%Y%m%dT%H%M%SZ}"]

    return [
        "BEGIN:VEVENT",
        f"UID:{sleutel}@ink-voetbal",
        f"DTSTAMP:{nu:%Y%m%dT%H%M%SZ}",
        *tijd,
        f"SUMMARY:{esc('⚽ ' + titel)}",
        *([f"LOCATION:{esc(locatie)}"] if locatie else []),
        f"DESCRIPTION:{esc(chr(10).join(regels))}",
        *(["STATUS:CANCELLED"] if status.lower() == "afgelast" else []),
        "END:VEVENT",
    ]


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--team", action="append", required=True,
                   help='teamnaam zonder club ("O9-2") of Sportlink-teamcode; mag vaker')
    p.add_argument("--uit", required=True, type=Path, help="pad van het .ics-bestand")
    p.add_argument("--cache", type=Path, default=Path(__file__).with_name("voetbal-cache.json"),
                   help="hier onthoudt het script gespeelde wedstrijden")
    p.add_argument("--dagen", type=int, default=120, help="zoveel dagen vooruit (standaard 120)")
    p.add_argument("--client-id", default=os.environ.get("SPORTLINK_CLIENT_ID"),
                   help="eigen Sportlink client_id van de club (anders via asv Dronten)")
    p.add_argument("--naam", default="Voetbal", help="naam van de kalender in het .ics-bestand")
    args = p.parse_args()

    nu = datetime.now(timezone.utc)
    codes = teamcodes(args.team, args.client_id)

    try:
        bewaard = json.loads(args.cache.read_text())
    except (OSError, ValueError):
        bewaard = {}

    wedstrijden = {}
    for code in codes:
        for w in ophalen("programma", {"teamcode": str(code), "aantaldagen": str(args.dagen)},
                         args.client_id):
            # Sommige clubs krijgen ook wedstrijden van andere teams mee.
            if code in (w.get("thuisteamid"), w.get("uitteamid")):
                wedstrijden[f"{w['wedstrijdcode']}-{code}"] = w

    # Gespeelde wedstrijden staan niet meer in het programma; die nemen we over
    # uit de vorige keer, tot ze 5 weken oud zijn. Toekomstige wedstrijden die
    # verdwenen zijn (bijv. geschrapt) laten we los.
    for sleutel, w in bewaard.items():
        if sleutel in wedstrijden or int(sleutel.split("-")[-1]) not in codes:
            continue
        start = datetime.strptime(w["wedstrijddatum"], "%Y-%m-%dT%H:%M:%S%z")
        if nu - BEWAAR_GESPEELD < start < nu:
            wedstrijden[sleutel] = w

    regels = ["BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//Ink//Voetbal//NL",
              "CALSCALE:GREGORIAN", f"X-WR-CALNAME:{esc(args.naam)}"]
    for sleutel, w in sorted(wedstrijden.items(), key=lambda x: x[1]["wedstrijddatum"]):
        regels += afspraak(sleutel, w, len(codes) > 1, nu)
    regels.append("END:VCALENDAR")

    # Eerst naar een tijdelijk bestand, zodat HA nooit een half bestand leest.
    args.uit.parent.mkdir(parents=True, exist_ok=True)
    tijdelijk = args.uit.with_name(args.uit.name + ".nieuw")
    tijdelijk.write_text("\r\n".join(vouw(r) for r in regels) + "\r\n", encoding="utf-8")
    tijdelijk.replace(args.uit)
    args.cache.parent.mkdir(parents=True, exist_ok=True)
    args.cache.write_text(json.dumps(wedstrijden, ensure_ascii=False))

    print(f"{len(wedstrijden)} wedstrijden in {args.uit}")


if __name__ == "__main__":
    try:
        main()
    except Exception as fout:  # één regel in de HA-melding, geen traceback
        print(f"Voetbal ophalen mislukt: {fout}", file=sys.stderr)
        sys.exit(1)

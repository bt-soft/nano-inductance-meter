# nano-inductance-meter

Induktivitásmérő **lecsengés (ringing) méréssel**: Arduino Nano + MCP6004 komparátor + Nokia 5110 LCD.
A kapcsolási rajz: [schematic.png](./schematic.png).

## A mérés elve

Az ismeretlen tekercs (Lx) egy ismert kondenzátorral (C) párhuzamos rezgőkört alkot. Ha a kört egy rövid impulzussal
„megrúgjuk”, csillapodó szinuszos rezgés (lecsengés) indul, melynek frekvenciája:

```
f = 1 / (2·π·√(L·C))      →      L = T² / (4·π²·C)
```

ahol `T = 1/f` a rezgés periódusideje. A C értéke ismert (kalibrálással pontosítva), így elég a periódusidőt megmérni.

## Hardver

| Láb | Funkció |
|-----|---------|
| D9 (PB1) | töltőimpulzus kimenet, 390 Ω-on át az Lx-hez; a másik vég VREF-en van. Mérés közben Hi-Z. |
| D8 (ICP1) | az MCP6004 „A” komparátorának kimenete (négyszögjel) → Timer1 input capture |
| D2 | kalibráló gomb (belső felhúzással, a gomb GND-re húz) |
| D3, D4, D5, D6, D7 | Nokia 5110: SCLK, DIN, D/C, CS, RST |

## A működés pontosan

### 1. Egy lövés (`measurePeriodTicks`)

1. **Töltés:** D9 magas szintre kerül, és 300 µs-ig (`PULSE_US`) kimenetként tölti az Lx-et a 390 Ω-os ellenálláson át.
2. **Elengedés:** D9 Hi-Z állapotba kapcsol, ekkor az Lx ∥ C kör szabadon lecseng.
3. **Élek rögzítése:** a Timer1 16 MHz-es órajellel (1 tick = 62,5 ns) fut, zajszűrővel (ICNC1), felfutó élre
   triggerelve. A komparátor kimenetének minden felfutó élénél megszakítás (`TIMER1_CAPT_vect`) fut le, ami:
   - az első él után elmenti az egymást követő élek közti időközt (tickben),
   - összegzi az időközöket (`sumTicks`), és követi a legkisebbet (`minD`) és a legnagyobbat (`maxD`).
4. **Gyűjtés vége:** legfeljebb 12 élt (`MAX_EDGES`) gyűjt, vagy 10 ms (`TIMEOUT_US`) után leáll.
5. **Várakozás:** 2 ms szünet, hogy a maradék lecsengés elhaljon a következő lövés előtt.
6. **Érvényesség:** a lövés érvényes, ha
   - legalább 4 él volt (`MIN_EDGES`), és
   - `maxD / minD ≤ 5/4` (`MAX_JITTER_NUM/DEN`), vagyis nincs kihagyott vagy zajos él.

   Érvényes lövésnél az eredmény: `sumTicks / (élek száma − 1)`, azaz az átlagos periódusidő tickben.
   Érvénytelennél 0.

### 2. Egy mérés (`measurePeriod`)

- 5 lövést végez (`SHOTS`), és az érvényesek átlagát veszi.
- Legalább 3 érvényes lövés kell (`MIN_VALID_SHOTS`), különben a mérés sikertelen (0).
- Az átlagot elosztja 16 MHz-cel, így másodpercben kapja a periódusidőt (`T`).

### 3. Simítás (`smoothPeriod`)

A mérések ingadozásának csökkentésére a periódusidőre exponenciális mozgóátlag van:

```
avgT += 0.04 · (T − avgT)          (AVG_ALPHA)
```

- Ha a mérés sikertelen (`T = 0`), az átlag nullázódik.
- Ha az új érték több mint 5%-kal tér el az átlagtól (`AVG_RESET_RATIO`), például másik tekercset csatlakoztattak,
  az átlag azonnal az új értékre ugrik.
- Kisebb `AVG_ALPHA` = simább, de lassabban követő kijelzés.

### 4. Kijelzés (`drawScreen`)

- Az induktivitást `L = T² / (4·π²·Cx)` adja, ahol `Cx` az effektív kapacitás (lásd Kalibráció).
- 1 mH-tól mH-ban (1 vagy 2 tizedesjeggyel), alatta µH-ban (`uH` felirat, 0–2 tizedesjeggyel) jelenik meg.
- Alatta a frekvencia (`f = 1/T`): 1 kHz-től kHz-ben, alatta Hz-ben.
- Ha nincs érvényes mérés: `---` és `Nincs lecsengo jel`, valamint hibakeresési adatok
  (`el:` az élek száma, `T:` a legkisebb–legnagyobb időköz tickben).
- A mérés minden ciklusban fut, de az LCD-t a villogás elkerülésére csak `DISPLAY_INTERVAL_MS` (800 ms)
  időközönként rajzolja újra.

## Kalibráció

A párhuzamos kondenzátor tényleges értéke (tűrés, parazita kapacitások, komparátor bemenet) eltér a névlegestől, ezért a
mérés pontossága az effektív `Cx` kalibrálásával javítható. Ehhez egy **ismert induktivitású** tekercs kell
(`L_CAL`, alapértelmezetten **47 µH**).

### Lépések

1. Csatlakoztasd az ismert (`L_CAL` értékű) tekercset a mérőpontokra.
2. Nyomd meg a kalibráló gombot (D2). A kijelzőn megjelenik: `Kalibralas / Kapcsold az / 47.0 uH-t`.
3. A kijelzés után 3 másodpercet vár, hogy elengedhesd a gombot és a tekercs a helyén legyen. **Tartsd
   a gombot csak röviden:** ha a mérési ciklus végén még lenyomva van, a kalibráció újraindul.
4. A készülék elvégzi a mérést (`measurePeriod`), majd kiszámolja az effektív kapacitást:

   ```
   Cx = T² / (4·π²·L_CAL)
   ```

5. Sikeres mérésnél kiírja `Kesz` és a `C=… nF` értéket, és **EEPROM-ba menti**. Sikertelen mérésnél (`Kal. hiba!`,
   `el:` az élek száma) a korábbi érték marad.
6. 2 másodperc után visszatér a normál mérésre.

### Tárolás az EEPROM-ban

| Cím | Méret | Tartalom |
|-----|-------|----------|
| 0 | 2 bájt | `EE_MAGIC` = `0xC1A6` (az adat érvényességét jelzi) |
| 2 | 4 bájt | `Cx` (float, farad) |

Indításkor (`loadCal`) az érték csak akkor kerül felhasználásra, ha a magic érték egyezik, és `Cx` a névleges
`C_NOM` 0,5–2-szerese között van. Ha nincs érvényes kalibráció, a névleges `C_NOM` (465 nF) az alapértelmezett érték.

### Beállítandó konstansok

A [src/main.cpp](./src/main.cpp) elején:

| Konstans | Jelentés | Alapérték |
|----------|----------|-----------|
| `C_NOM` | a párhuzamos kondenzátor névleges értéke (F) | `465e-9` |
| `L_CAL` | a kalibráló tekercs induktivitása (H) | `47e-6` |

Másik kondenzátor vagy kalibráló tekercs használatakor ezeket állítsd át, majd töltsd fel újra a programot és
kalibrálj.

## Hangolható paraméterek

| Konstans | Hatás | Alapérték |
|----------|-------|-----------|
| `PULSE_US` | töltőimpulzus hossza (µs) | 300 |
| `MAX_EDGES` | lövésenként gyűjtött élek száma | 12 |
| `MIN_EDGES` | érvényes lövéshez szükséges élek | 4 |
| `TIMEOUT_US` | lövésenkénti maximális mérési idő (µs) | 10000 |
| `SHOTS` | lövések száma egy mérésben | 5 |
| `MIN_VALID_SHOTS` | érvényes mérés minimum lövésszáma | 3 |
| `AVG_ALPHA` | mozgóátlag súlya (kisebb = simább) | 0,04 |
| `AVG_RESET_RATIO` | ekkora relatív ugrásnál újraindul az átlag | 0,05 |
| `DISPLAY_INTERVAL_MS` | LCD frissítési időköz (ms) | 800 |

## Build

PlatformIO (`nanoatmega328`, Arduino keretrendszer):

```
pio run
pio run -t upload
```

Függőségek: Adafruit PCD8544 Nokia 5110 LCD library, Adafruit GFX Library.

# nano-inductance-meter

Induktivitásmérő **lecsengés (ringing) méréssel**: Arduino Nano + MCP6004 komparátor + Nokia 5110 LCD. A kapcsolási rajz: [schematic.png](./schematic.png).

## A mérés elve

Az ismeretlen tekercs (Lx) egy ismert (pl.: arra alkalmas multiméterrel kimért) kondenzátorral (C) párhuzamos rezgőkört alkot. Ha a kört egy rövid impulzussal
„megrúgjuk”, csillapodó szinuszos rezgés (lecsengés) indul, melynek frekvenciája:

```
f = 1 / (2·π·√(L·C))      →      L = T² / (4·π²·C)
```

ahol `T = 1/f` a rezgés periódusideje. A C értéke ismert (kalibrálással pontosítva), így elég a periódusidőt megmérni.

## Hardver

| Láb | Funkció |
|-----|---------|
| D9 (PB1) | töltőimpulzus kimenet, 390 Ω-on át az Lx-hez; a másik vég VREF-en van. Mérés  közben Hi-Z. |
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

## Mérési tartomány

A számok a névleges `C = 465 nF` (multiméterrel kimért) párhuzamos kondenzátorra vonatkoznak. 
Ez elméleti és becsült, nem mért tartomány: a valós határokat a tekercs ohmos vesztesége (Q-ja), a komparátor és a huzalozás befolyásolja.

**Gyakorlati tartomány: kb. 2 µH … 300 mH**, a rezgési frekvencia ekkor kb. 150 kHz … 400 Hz.

### Az alsó határ (kis L, nagy frekvencia)

- **Megszakítás-késleltetés:** minden felfutó él egy megszakítást (ISR) indít, ami kb. 4–5 µs-ig fut. A rezgés
  periódusidejének ennél lényegesen hosszabbnak kell lennie, különben élek maradnak ki, és a jitter-ellenőrzés
  (`maxD/minD ≤ 5/4`) a lövést eldobja. Ez kb. **150–200 kHz** felső frekvenciahatárt ad.
- **Komparátor sávszélessége:** az MCP6004 erősítés-sávszélesség szorzata kb. 1 MHz, a slew rate kb. 0,6 V/µs,
  így 100–200 kHz fölött a négyszögjel torzul.
- **Időfelbontás:** 1 tick = 62,5 ns. 230 kHz-en a periódus kb. 69 tick, tehát egy tick kb. 1,5% hiba (L-ben kb. 3%).
  Az élek közti időközök átlagolása (`sumTicks / (n−1)`) ezt javítja, de a pontosság lefelé romlik.
- **Parazita induktivitás és kapacitás:** néhány µH alatt a bekötés, a próbapanel és a vezetékek saját induktivitása
  már összemérhető a mérendővel.

### A felső határ (nagy L, kis frekvencia)

- **Időtúllépés:** a gyűjtés 10 ms-ig tart (`TIMEOUT_US`), és legalább 4 él kell (`MIN_EDGES`). Az első él a
  letöltés után legfeljebb egy periódussal később érkezik, ezért a periódusidő legfeljebb kb. 2,5 ms lehet
  (**kb. 400 Hz → L ≈ 0,34 H**). Ennél lassabb lecsengésből nem gyűlik össze elég él.
- **Timer1 túlcsordulás:** a 16 bites számláló 16 MHz-en 4,096 ms alatt fordul körbe, az időközt `uint16_t`-ben
  számoljuk. Egy periódus nem lehet hosszabb ennél (**kb. 244 Hz → L ≈ 0,78 H**). A 10 ms-os időtúllépés ennél
  szigorúbb, ezért az dönt.
- **Gyenge gerjesztés:** a töltőimpulzus 300 µs hosszú, 390 Ω-on át. Nagy L-nél az áram lassan nő
  (időállandó `L/R`), ezért kevés energia kerül a körbe, és a lecsengés kicsi és gyorsan elhal.
  Kb. 100 mH fölött ezért a mérés fokozatosan bizonytalanabb, és a jel eltűnhet a komparátor küszöbe alatt.
- **Csillapítás:** a tekercs soros ellenállása és a veszteségei miatt a rezgés néhány periódus alatt elhalhat.
  Rossz Q-jú (például vasmagos, nagy ohmos) tekercsnél a 4 élt sem sikerül összegyűjteni.

### Induktivitás és mérési frekvencia (C = 465 nF)

| L | f | T | Megjegyzés |
|---|---|---|------------|
| 1 µH | 233 kHz | 4,3 µs | a határ fölött, megbízhatatlan |
| 2,2 µH | 157 kHz | 6,4 µs | határeset |
| 4,7 µH | 108 kHz | 9,3 µs | |
| 10 µH | 74 kHz | 13,6 µs | |
| 47 µH | 34 kHz | 29,4 µs | a kalibráló tekercs (`L_CAL`) |
| 100 µH | 23 kHz | 42,9 µs | |
| 470 µH | 10,8 kHz | 93 µs | |
| 1 mH | 7,4 kHz | 135 µs | |
| 4,7 mH | 3,4 kHz | 294 µs | |
| 10 mH | 2,3 kHz | 429 µs | |
| 47 mH | 1,08 kHz | 929 µs | |
| 100 mH | 738 Hz | 1,36 ms | gyengülő jel |
| 470 mH | 340 Hz | 2,9 ms | a 10 ms-os időtúllépés miatt már nem mérhető megbízhatóan |
| 1 H | 233 Hz | 4,3 ms | nem mérhető |

A frekvencia kiszámítása másik kondenzátorral: `f = 1 / (2·π·√(L·C))`. A mérési tartomány a frekvenciahatárok
(kb. 400 Hz … 150 kHz) alapján eltolható: kisebb C-nél a rezgés gyorsabb, így a nagy L-ek jobban mérhetők, a kis L-ek
viszont kikerülnek a tartományból. Nagyobb C-nél ennek az ellenkezője igaz. Ezért van egy másik lehetséges
`C_NOM = 82 nF` érték kikommentelve a kódban: ezzel a tartomány a nagyobb induktivitások felé tolódik.

## Kalibráció

A párhuzamos kondenzátor tényleges értéke (tűrés, parazita kapacitások, komparátor bemenet) eltér a névlegestől, ezért a
mérés pontossága az effektív `Cx` kalibrálásával javítható. Ehhez egy **ismert induktivitású** tekercs kell
(`L_CAL`, alapértelmezetten **47 µH**, ilyet sikerült vennem, elhiszem hogy ekkora az értéke...).

### Lépések

1. Csatlakoztasd az ismert (`L_CAL` értékű) tekercset a mérőpontokra.
2. Nyomd meg a kalibráló gombot (D2). A kijelzőn megjelenik: `Kalibralas / Kapcsold az / 47.0 uH-t`.
3. A kijelzés után 3 másodpercet vár, hogy elengedhesd a gombot és a tekercs a helyén legyen. 
   **A gombot csak röviden nyomd:** ha a mérési ciklus végén még lenyomva van, a kalibráció újraindul.
4. A készülék elvégzi a mérést (`measurePeriod`), majd kiszámolja az effektív kapacitást:

   ```
   Cx = T² / (4·π²·L_CAL)
   ```

5. Sikeres mérésnél kiírja `Kesz` és a `C=… nF` értéket, és **EEPROM-ba menti**. Sikertelen mérésnél (`Kal. hiba!`, `el:` az élek száma) a korábbi érték marad.
6. 2 másodperc után visszatér a normál mérésre.

### Tárolás az EEPROM-ban

| Cím | Méret | Tartalom |
|-----|-------|----------|
| 0 | 2 bájt | `EE_MAGIC` = `0xC1A6` (az adat érvényességét jelzi) |
| 2 | 4 bájt | `Cx` (float, farad) |

Indításkor (`loadCal`) az érték csak akkor kerül felhasználásra, ha a magic érték egyezik, és `Cx` a névleges `C_NOM` 0,5–2-szerese között van. Ha nincs érvényes kalibráció, a névleges `C_NOM` (465 nF) az alapértelmezett érték.

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

// Induktivitásmérő lecsengés (ringing) méréssel: Arduino Nano + MCP6004 + Nokia
// 5110 A D9 rövid ideig 390 ohmon át árammal tölti az Lx-et (a másik vég
// VREF-en), majd Hi-Z-be kapcsol.
// Az Lx || C kör ezután f = 1/(2*pi*sqrt(L*C)) frekvencián cseng le.
// Az MCP6004 A komparátora négyszöggé alakítja (D8 = ICP1),
// a Timer1 input capture a periódusidőt méri. Lx = T^2 / (4*pi^2*C).
#include <Arduino.h>
#include <EEPROM.h>

#include <Adafruit_GFX.h>
#include <Adafruit_PCD8544.h>

// ---------------------------------------------------------------------------
// Lábkiosztás
// ---------------------------------------------------------------------------
// D9 = PB1: töltőimpulzus kimenet, D8 = ICP1: komparátor kimenet
const uint8_t PIN_BTN = 2;     // kalibráló gomb (GND-re húz)
const uint8_t PIN_ICP = 8;     // komparátor kimenet (Timer1 input capture)
const uint8_t PULSE_BIT = PB1; // D9 a PORTB/DDRB regiszterekben

// Nokia 5110: SCLK, DIN, D/C, CS, RST
Adafruit_PCD8544 lcd(3, 4, 5, 6, 7);

// ---------------------------------------------------------------------------
// Kalibráció és EEPROM
// ---------------------------------------------------------------------------
// const double C_NOM = 82e-9;    // másik lehetséges párhuzamos kondenzátor
const double C_NOM = 465e-9;      // a párhuzamos kondenzátor névleges értéke (F)
const double L_CAL = 47e-6;       // a kalibráláshoz használt ismert induktivitás (H)
double Cx = C_NOM;                // effektív kapacitás, kalibrálással felülíródik (EEPROM)
const int EE_ADDR = 0;            // EEPROM kezdőcím a kalibrációs adatokhoz
const uint16_t EE_MAGIC = 0xC1A6; // a kalibrációs adat érvényességét jelző érték

// ---------------------------------------------------------------------------
// Mérési paraméterek
// ---------------------------------------------------------------------------
const double F_CPU_D = 16.0e6;          // CPU órajel (Hz), a Timer1 ennyivel számol
const uint16_t PULSE_US = 300;          // töltőimpulzus hossza (µs)
const uint8_t MAX_EDGES = 12;           // ennyi felfutó élt gyűjtünk ciklusonként
const uint8_t MIN_EDGES = 4;            // ennyi él kell az érvényes mérési ciklushoz
const unsigned long TIMEOUT_US = 10000; // max. mérési idő ciklusonként (µs)
const uint8_t MEASUREMENT_CYCLES = 5;   // ennyi ciklust átlagolunk egy méréshez
const uint8_t MIN_VALID_CYCLES = 3;     // ennyi érvényes ciklus kell egy méréshez

// Az időközök max/min aránya ennél nagyobb nem lehet (MAX_JITTER_NUM/DEN = 5/4)
const uint8_t MAX_JITTER_NUM = 5;
const uint8_t MAX_JITTER_DEN = 4;

// ---------------------------------------------------------------------------
// Kijelzés és simítás
// ---------------------------------------------------------------------------
const unsigned long DISPLAY_INTERVAL_MS = 800; // LCD frissítési intervallum (ms)
const double AVG_ALPHA = 0.04;                 // mozgóátlag súlya (kisebb = simább)
const double AVG_RESET_RATIO = 0.05;           // ekkora relatív ugrásnál újraindul az átlag

// ---------------------------------------------------------------------------
// Megszakításból írt adatok és debug információk
// ---------------------------------------------------------------------------
volatile uint16_t lastCap, minD, maxD;
volatile uint32_t sumTicks;
volatile uint8_t nEdges;
uint8_t dbgEdges;        // utolsó mérési ciklus éleinek száma
uint16_t dbgMin, dbgMax; // utolsó mérési ciklus legkisebb/legnagyobb időköze (órajel)

// ---------------------------------------------------------------------------
// Mérés
// ---------------------------------------------------------------------------

/**
 * Timer1 input capture megszakítás kezelője
 * Felfutó éleket számol, és tárolja az időközöket
 */
ISR(TIMER1_CAPT_vect) {
    uint16_t c = ICR1;
    if (nEdges >= MAX_EDGES) {
        return;
    }

    // az első él után kezdjük az időközök számolását
    if (nEdges > 0) {
        uint16_t d = c - lastCap;
        sumTicks += d;
        if (d < minD) {
            minD = d;
        }
        if (d > maxD) {
            maxD = d;
        }
    }
    lastCap = c;
    nEdges++;
}

/**
 * Timer1 inicializálása input capture-hez
 */
void timerInit() {
    TCCR1A = 0;
    TCCR1B = _BV(ICNC1) | _BV(ICES1) | _BV(CS10); // zajszűrő, felfutó él, f_cpu
    TIMSK1 = 0;
}

/**
 * Egy lecsengés periódusidő mérése órajelben
 * 0-t ad, ha nincs elég él
 */
double measurePeriodTicks() {
    // töltőimpulzus: D9 magas, 390 ohmon át tölti az Lx-et
    PORTB |= _BV(PULSE_BIT);
    DDRB |= _BV(PULSE_BIT);
    delayMicroseconds(PULSE_US);

    // töltőimpulzus vége (D9 Hi-Z), most kezdjük a mérést
    noInterrupts();
    DDRB &= ~_BV(PULSE_BIT);
    PORTB &= ~_BV(PULSE_BIT);

    nEdges = 0;
    sumTicks = 0;
    minD = 0xFFFF;
    maxD = 0;
    TIFR1 = _BV(ICF1);   // input capture flag törlése
    TIMSK1 = _BV(ICIE1); // input capture interrupt engedélyezése
    interrupts();

    // várakozás az élekre vagy az időtúllépésre
    const unsigned long t0 = micros();
    while (micros() - t0 < TIMEOUT_US && nEdges < MAX_EDGES) {
    }

    TIMSK1 = 0; // input capture interrupt letiltása

    // konzisztens pillanatkép a megszakításban írt adatokról
    noInterrupts();
    const uint8_t n = nEdges;
    const uint32_t s = sumTicks;
    const uint16_t mn = minD;
    const uint16_t mx = maxD;
    interrupts();

    delay(2); // a maradék lecsengés elhalása

    // debug adatok az LCD-s hibakijelzéshez
    dbgEdges = n;
    dbgMin = mn;
    dbgMax = mx;

    if (n < MIN_EDGES) {
        return 0;
    }
    // kihagyott vagy zajos él: az időközök túl eltérőek
    if ((uint32_t)mx * MAX_JITTER_DEN > (uint32_t)mn * MAX_JITTER_NUM) {
        return 0;
    }
    return (double)s / (n - 1);
}

/**
 * Egy lecsengés periódusidő mérése másodpercben
 * Több mérési ciklust átlagol; 0-t ad, ha túl kevés az érvényes ciklus
 */
double measurePeriod() {
    double sum = 0;
    uint8_t ok = 0;

    for (uint8_t i = 0; i < MEASUREMENT_CYCLES; i++) {
        const double t = measurePeriodTicks();
        if (t > 0) {
            sum += t;
            ok++;
        }
    }
    if (ok < MIN_VALID_CYCLES) {
        return 0;
    }
    return sum / ok / F_CPU_D; // átváltás másodpercre
}

/**
 * Induktivitás számítása a periódusidőből: L = T^2 / (4*pi^2*C)
 */
double inductanceFromPeriod(double T, double C) { return T * T / (4.0 * PI * PI * C); }

/**
 * Mozgóátlag a periódusidőre
 * Nagy ugrásnál (másik tekercs) vagy hibánál újraindul
 */
double smoothPeriod(double T) {
    static double avgT = 0;
    if (T == 0) {
        avgT = 0;
    } else if (avgT == 0 || fabs(T - avgT) > AVG_RESET_RATIO * avgT) {
        avgT = T;
    } else {
        avgT += AVG_ALPHA * (T - avgT);
    }
    return avgT;
}

// ---------------------------------------------------------------------------
// Kalibráció
// ---------------------------------------------------------------------------

/**
 * Kalibrációs adatok betöltése az EEPROM-ból
 */
void loadCal() {
    uint16_t magic;
    float c;
    EEPROM.get(EE_ADDR, magic);
    if (magic != EE_MAGIC) {
        return;
    }
    EEPROM.get(EE_ADDR + 2, c);
    // csak hihető érték esetén írjuk felül a névlegeset
    if (c > 0.5 * C_NOM && c < 2.0 * C_NOM) {
        Cx = c;
    }
}

/**
 * Kalibrációs függvény
 * Ismert L_CAL mellett a periódusidőből számítja az effektív C-t
 */
void calibrate() {
    lcd.clearDisplay();
    lcd.setCursor(0, 0);
    lcd.println("Kalibralas");
    lcd.println("Kapcsold az");
    lcd.print(L_CAL * 1e6, 1);
    lcd.println(" uH-t");
    lcd.display();
    delay(3000);

    const double T = measurePeriod();

    lcd.clearDisplay();
    lcd.setCursor(0, 0);
    if (T == 0) {
        lcd.println("Kal. hiba!");
        lcd.print("el:");
        lcd.println(dbgEdges);
    } else {
        Cx = T * T / (4.0 * PI * PI * L_CAL);
        EEPROM.put(EE_ADDR, EE_MAGIC);
        EEPROM.put(EE_ADDR + 2, (float)Cx);
        lcd.println("Kesz");
        lcd.print("C=");
        lcd.print(Cx * 1e9, 1);
        lcd.println("nF");
    }
    lcd.display();
    delay(2000);
}

// ---------------------------------------------------------------------------
// Kijelzés
// ---------------------------------------------------------------------------

/**
 * Induktivitás kiírása: 1 mH felett mH-ban, alatta µH-ban
 * (az LCD ékezet nélküli "uH" feliratot használ)
 */
void printInductance(double L) {
    lcd.setTextSize(2);
    if (L >= 1e-3) {
        lcd.print(L * 1e3, L >= 10e-3 ? 1 : 2);
        lcd.println("mH");
    } else {
        lcd.print(L * 1e6, L < 10e-6 ? 2 : (L < 100e-6 ? 1 : 0));
        lcd.println("uH");
    }
    lcd.setTextSize(1);
}

/**
 * Frekvencia kiírása: 1 kHz felett kHz-ben, alatta Hz-ben
 */
void printFrequency(double f) {
    lcd.print("f=");
    if (f >= 1000) {
        lcd.print(f / 1e3, f >= 100e3 ? 1 : 2);
        lcd.println(" kHz");
    } else {
        lcd.print(f, 0);
        lcd.println(" Hz");
    }
}

/**
 * Hibakijelzés: nincs érvényes lecsengés, debug adatokkal
 */
void printNoSignal() {
    lcd.setTextSize(2);
    lcd.println("---");
    lcd.setTextSize(1);
    lcd.println("Nincs lecsengo jel");
    lcd.print("el:"); // élek száma
    lcd.print(dbgEdges);
    lcd.print(" T:"); // legkisebb és legnagyobb időköz
    lcd.print(dbgMin);
    lcd.print("-");
    lcd.println(dbgMax);
}

/**
 * A teljes mérési képernyő kirajzolása
 */
void drawScreen(double T) {
    lcd.clearDisplay();
    lcd.setCursor(0, 0);
    lcd.println("L-meter");

    if (T == 0) {
        printNoSignal();
    } else {
        printInductance(inductanceFromPeriod(T, Cx));
        printFrequency(1.0 / T);
    }
    lcd.display();
}

// ---------------------------------------------------------------------------
// Arduino belépési pontok
// ---------------------------------------------------------------------------

/**
 * Arduino beállítási függvény
 * Inicializálja a lábakat, LCD-t, timert és betölti a kalibrációs adatokat
 */
void setup() {
    pinMode(PIN_BTN, INPUT_PULLUP);
    pinMode(PIN_ICP, INPUT);  // komparátor jel fogadása
    DDRB &= ~_BV(PULSE_BIT);  // D9 bemenet (Hi-Z)
    PORTB &= ~_BV(PULSE_BIT); // D9 pull-up letiltása

    lcd.begin();
    lcd.setContrast(65);
    lcd.setTextSize(1);
    lcd.setTextColor(BLACK);
    lcd.setRotation(2); // LCD elforgatása 180 fokkal

    timerInit();
    loadCal();
}

/**
 * Arduino fő ciklus függvény
 * Ellenőrzi a kalibrációs gombot, méri a periódusidőt és frissíti az LCD-t
 */
void loop() {
    // kalibrációs gomb ellenőrzése
    if (digitalRead(PIN_BTN) == LOW) {
        calibrate();
    }

    const double T = smoothPeriod(measurePeriod());

    // az LCD-t csak ritkábban frissítjük, hogy ne villogjon
    static unsigned long lastDisplayUpdate = 0;
    const unsigned long now = millis();
    if (now - lastDisplayUpdate < DISPLAY_INTERVAL_MS) {
        return;
    }
    lastDisplayUpdate = now;

    drawScreen(T);
}

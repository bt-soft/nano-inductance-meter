// Induktivitásmérő lecsengés (ringing) méréssel: Arduino Nano + MCP6004 + Nokia
// 5110 A D9 rövid ideig 390 ohmon át árammal tölti az Lx-et (a másik vég
// VREF-en), majd Hi-Z-be kapcsol.
// Az Lx || C kör ezután f = 1/(2*pi*sqrt(L*C)) frekvencián cseng le.
// Az MCP6004 A komparátora négyszöggé alakítja (D8 = ICP1),
// a Timer1 input capture a periódusidőt méri. Lx = T^2 / (4*pi^2*C).
#include <Arduino.h>
#include <EEPROM.h>

#include <Adafruit_GFX.h>

#include <Fonts/FreeSerifBold18pt7b.h>
#include <Fonts/Org_01.h>
#include <Fonts/Picopixel.h>

#include <Adafruit_PCD8544.h>

const uint8_t PIN_BTN = 2; // kalibráló gomb (GND-re húz)
// D9 = PB1: pulzus kimenet, D8 = ICP1: komparátor kimenet

// Nokia 5110: SCLK, DIN, D/C, CS, RST
Adafruit_PCD8544 lcd(3, 4, 5, 6, 7);

// const double C_NOM = 82e-9;   // a párhuzamos kondenzátor névleges értéke
const double C_NOM = 465e-9;      // a párhuzamos kondenzátor névleges értéke
const double L_CAL = 47e-6;       // a kalibráláshoz használt ismert induktivitás (H)
double Cx = C_NOM;                // kalibrálással felülíródik (EEPROM)
const int EE_ADDR = 0;            // EEPROM kezdőcím a kalibrációs adatokhoz
const uint16_t EE_MAGIC = 0xC1A6; // EEPROM-ban a kalibrációs adat érvényességét jelző érték
const double F_CPU_D = 16.0e6;    // CPU órajel Hz-ben

const uint16_t PULSE_US = 300;          // töltőimpulzus hossza
const uint8_t MAX_EDGES = 12;           // ennyi felfutó élt gyűjtünk
const uint8_t MIN_EDGES = 4;            // ennyi él kell az érvényes méréshez
const unsigned long TIMEOUT_US = 10000; // max. mérési idő lövésenként
const uint8_t SHOTS = 5;                // ennyi próbálkozás a periódusidő mérésére

volatile uint16_t lastCap, minD, maxD;
volatile uint32_t sumTicks;
volatile uint8_t nEdges;
uint8_t dbgEdges;
uint16_t dbgMin, dbgMax;

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
        if (d < minD)
            minD = d;
        if (d > maxD)
            maxD = d;
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
    PORTB |= _BV(PB1); // D9 magas, 390 ohmon át tölti az Lx-et
    DDRB |= _BV(PB1);  // D9 kimenet, töltőimpulzus indítása

    // várakozás a töltőimpulzus idejére
    delayMicroseconds(PULSE_US);

    // töltőimpulzus vége, most kezdjük a mérést
    noInterrupts();
    DDRB &= ~_BV(PB1);  // Hi-Z
    PORTB &= ~_BV(PB1); // D9 Hi-Z, töltőimpulzus vége

    nEdges = 0;
    sumTicks = 0;
    minD = 0xFFFF;
    maxD = 0;
    TIFR1 = _BV(ICF1);   // input capture flag törlése
    TIMSK1 = _BV(ICIE1); // input capture interrupt engedélyezése
    interrupts();

    unsigned long t0 = micros();
    while (micros() - t0 < TIMEOUT_US && nEdges < MAX_EDGES) {
    }

    TIMSK1 = 0; // input capture interrupt letiltása
    noInterrupts();

    uint8_t n = nEdges;
    uint32_t s = sumTicks;
    uint16_t mn = minD, mx = maxD;
    interrupts();

    delay(2); // a maradék lecsengés elhalása
    dbgEdges = n;
    dbgMin = mn;
    dbgMax = mx;
    // debug adatok kiírása az LCD-re vagy soros monitorra (opcionális)
    if (n < MIN_EDGES)
        return 0;
    if ((uint32_t)mx * 4 > (uint32_t)mn * 5)
        return 0; // kihagyott/zajos él
    return (double)s / (n - 1);
}

/**
 * Egy lecsengés periódusidő mérése másodpercben
 * Többször próbálkozik, és átlagot számol
 */
double measurePeriod() {
    double sum = 0;
    uint8_t ok = 0;

    // Többször próbálkozunk a periódusidő mérésével és átlagot számolunk
    for (uint8_t i = 0; i < SHOTS; i++) {
        double t = measurePeriodTicks();
        if (t > 0) {
            sum += t;
            ok++;
        }
    }
    // Ha túl kevés sikeres mérés van, hibát jelezünk
    if (ok < 3) {
        return 0;
    }
    // Átlagoljuk a sikeres méréseket és átváltjuk másodpercre
    return sum / ok / F_CPU_D; // másodperc
}

/**
 * Kalibrációs adatok betöltése az EEPROM-ból
 */
void loadCal() {
    uint16_t m;
    float c;
    EEPROM.get(EE_ADDR, m);
    if (m != EE_MAGIC) {
        return;
    }
    EEPROM.get(EE_ADDR + 2, c); // kalibrációs konstans betöltése
    if (c > 0.5 * C_NOM && c < 2.0 * C_NOM) {
        Cx = c; // csak érvényes érték esetén állítjuk be
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

    // Periódusidő mérése
    double T = measurePeriod();

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

/**
 * Arduino beállítási függvény
 * Inicializálja a lábakat, LCD-t, timert és betölti a kalibrációs adatokat
 */
void setup() {
    // Kalibráció button pin beállítása
    pinMode(PIN_BTN, INPUT_PULLUP);
    pinMode(8, INPUT);  // 8 láb bemenetként a komparátor jel fogadására
    DDRB &= ~_BV(PB1);  // PB1 (D9) bemenetként
    PORTB &= ~_BV(PB1); // PB1 (D9) pull-up letiltása

    // LCD inicializálása
    lcd.begin();
    lcd.setContrast(30);
    lcd.setTextSize(1);
    lcd.setTextColor(BLACK);
    lcd.setRotation(2); // LCD elforgatása 180 fokkal
    timerInit();        // Timer inicializálása
    loadCal();          // Kalibrációs adatok betöltése
}

/**
 * Arduino fő ciklus függvény
 * Ellenőrzi a kalibrációs gombot, méri a periódusidőt és frissíti az LCD-t
 */
void loop() {

    // Kalibrációs gomb ellenőrzése
    if (digitalRead(PIN_BTN) == LOW) {
        calibrate();
    }

    //
    double T = measurePeriod();

    // Mozgóátlag: nagy ugrásnál (másik tekercs) újraindul
    static double avgT = 0;
    if (T == 0) {
        avgT = 0;
    } else if (avgT == 0 || fabs(T - avgT) > 0.05 * avgT) {
        avgT = T;
    } else {
        avgT += 0.15 * (T - avgT);
    }
    T = avgT;
    lcd.clearDisplay();
    lcd.setCursor(0, 0);
    lcd.println("L-meter");
    if (T == 0) {
        lcd.setTextSize(2);
        lcd.println("---");
        lcd.setTextSize(1);
        lcd.println("Nincs lecsengo jel");
        lcd.print("el:"); // élek száma
        lcd.print(dbgEdges);
        lcd.print(" T:");  // periódusidő
        lcd.print(dbgMin); // minimum periódusidő
        lcd.print("-");
        lcd.println(dbgMax); // maximum periódusidő
    } else {
        double L = T * T / (4.0 * PI * PI * Cx); // induktancia számítása
        lcd.setTextSize(2);

        // Ha az induktancia nagyobb vagy egyenlő 1 mH-nál, akkor mH-ban jelenítjük meg
        if (L >= 1e-3) {
            lcd.print(L * 1e3, L >= 10e-3 ? 1 : 2);
            lcd.println("mH");
        } else { // Ha az induktancia kisebb 1 mH-nál, akkor uH-ban jelenítjük meg
            lcd.print(L * 1e6, L < 10e-6 ? 2 : (L < 100e-6 ? 1 : 0));
            lcd.println("uH");
        }

        // Frekvencia kiírása
        lcd.setTextSize(1);
        double f = 1.0 / T;
        lcd.print("f=");

        // Ha a frekvencia nagyobb vagy egyenlő 1000 Hz-nél, akkor kHz-ban jelenítjük meg
        if (f >= 1000) {
            lcd.print(f / 1e3, f >= 100e3 ? 1 : 2);
            lcd.println(" kHz");
        } else {
            lcd.print(f, 0);
            lcd.println(" Hz");
        }
    }
    lcd.display();
}

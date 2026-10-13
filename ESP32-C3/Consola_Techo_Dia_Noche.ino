#include <Arduino_GFX_Library.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <math.h>

// ==================================================
// CONSOLA DE TECHO - ESP32-C3 ZERO
// Pantalla NV3007: 428 x 142; zona útil aproximada: y=10..131
// Dos temas: día (claro) y noche (oscuro)
// Pulsador: pulsación corta = brillo; larga = cambiar tema
// ==================================================

// ---------- Pines de pantalla (configuración comprobada) ----------
#define TFT_SCK   4
#define TFT_MOSI  6
#define TFT_CS    7
#define TFT_DC    2
#define TFT_RST   3

// IMPORTANTE: conectar BL/BLK de la pantalla a GPIO9 para regular brillo.
// No conectar BL/BLK simultáneamente a 3,3 V si se usa PWM.
#define TFT_BL    9

// Pulsador entre GPIO1 y GND
#define BUTTON_PIN 1

// ---------- LED RGB integrado ----------
#define LED_PIN   10
#define NUM_LEDS  1
Adafruit_NeoPixel pixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------- Bluetooth BLE ----------
#define SERVICE_UUID   "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_LED_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_DATA_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"

BLEServer *pServer = nullptr;
BLECharacteristic *pCharLED = nullptr;
BLECharacteristic *pCharData = nullptr;
bool deviceConnected = false;

// ---------- Pantalla ----------
Arduino_DataBus *bus = new Arduino_ESP32SPI(
  TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED
);

Arduino_GFX *gfx = new Arduino_NV3007(
  bus, TFT_RST, 1, false,
  142, 428, 12, 0, 14, 0,
  nv3007_279_init_operations,
  sizeof(nv3007_279_init_operations)
);

// ---------- Coordenadas ----------
const int16_t TOP = 10;
const int16_t BOTTOM = 131;
const int16_t EXT_X = 72;
const int16_t COMPASS_X = 214;
const int16_t INT_X = 356;

// ---------- Datos de prueba (todavía simulados) ----------
float tempExterior = 18.2;
float tempInterior = 24.2;
int humedadExterior = 60;
int humedadInterior = 50;
float rumbo = 45.0;
unsigned long ultimaActualizacion = 0;

// ---------- Temas y brillo ----------
enum TemaPantalla { TEMA_DIA, TEMA_NOCHE };
TemaPantalla temaActual = TEMA_DIA;

// Niveles de brillo de pantalla: 50 %, 75 %, 100 %.
const uint8_t nivelesBrillo[] = {128, 191, 255};
const uint8_t NUM_NIVELES_BRILLO = sizeof(nivelesBrillo) / sizeof(nivelesBrillo[0]);
uint8_t indiceBrillo = 1; // Arranca al 75 %

// Colores RGB565 por tema
struct Paleta {
  uint16_t fondo;
  uint16_t texto;
  uint16_t acento;
  uint16_t humedad;
  uint16_t aguja;
  uint16_t agujaSecundaria;
};

const Paleta PALETA_DIA = {
  0xFFFF, // blanco
  0x1082, // negro azulado
  0x055D, // azul
  0x055D, // gota y humedad
  0xF800, // punta roja
  0x7BEF  // cola gris
};

const Paleta PALETA_NOCHE = {
  0x0000, // negro
  0xFFFF, // blanco
  0x07FF, // cian
  0x07FF, // gota y humedad
  0xF800, // punta roja
  0xBDF7  // cola gris claro
};

const Paleta &paletaActual() {
  return (temaActual == TEMA_DIA) ? PALETA_DIA : PALETA_NOCHE;
}

// ---------- Texto centrado con FreeFonts ----------
void textoCentrado(const char *texto, int16_t centroX, int16_t y,
                   const GFXfont *fuente, uint16_t color) {
  gfx->setFont(fuente);
  gfx->setTextColor(color);

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(texto, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(centroX - (w / 2), y);
  gfx->print(texto);
}

// ---------- Dirección cardinal ----------
const char *obtenerDireccion(float grados) {
  if (grados >= 337.5 || grados < 22.5) return "N";
  if (grados < 67.5)  return "NE";
  if (grados < 112.5) return "E";
  if (grados < 157.5) return "SE";
  if (grados < 202.5) return "S";
  if (grados < 247.5) return "SO";
  if (grados < 292.5) return "O";
  return "NO";
}

// ---------- Gota simplificada para 428 x 142 ----------
void lineaGotaGruesa(int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint16_t color) {
  // Tres trazos paralelos para simular un contorno en negrita.
  gfx->drawLine(x1, y1, x2, y2, color);
  gfx->drawLine(x1 + 1, y1, x2 + 1, y2, color);
  gfx->drawLine(x1, y1 + 1, x2, y2 + 1, color);
}

void dibujarGota(int16_t x, int16_t y, uint16_t color) {
  // Contorno reforzado para mejorar la legibilidad en la pantalla pequeña.
  lineaGotaGruesa(x + 10, y,      x + 3,  y + 10, color);
  lineaGotaGruesa(x + 3,  y + 10, x + 1,  y + 15, color);
  lineaGotaGruesa(x + 1,  y + 15, x + 3,  y + 20, color);
  lineaGotaGruesa(x + 3,  y + 20, x + 7,  y + 23, color);
  lineaGotaGruesa(x + 7,  y + 23, x + 12, y + 23, color);
  lineaGotaGruesa(x + 12, y + 23, x + 17, y + 20, color);
  lineaGotaGruesa(x + 17, y + 20, x + 19, y + 15, color);
  lineaGotaGruesa(x + 19, y + 15, x + 17, y + 10, color);
  lineaGotaGruesa(x + 17, y + 10, x + 10, y, color);
}

// ---------- Título con líneas laterales ----------
void dibujarTituloConLineas(const char *texto, int16_t centroX, int16_t baseline,
                            int16_t limiteIzquierdo, int16_t limiteDerecho,
                            uint16_t color) {
  gfx->setFont(&FreeSans9pt7b);
  gfx->setTextColor(color);

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(texto, 0, baseline, &x1, &y1, &w, &h);
  int16_t textoIzquierdo = centroX - (w / 2);
  int16_t textoDerecho = textoIzquierdo + w;
  int16_t yLinea = baseline - 7;
  const int16_t separacion = 5;

  // La línea se dibuja a ambos lados y queda visualmente interrumpida por el título.
  if (textoIzquierdo - separacion > limiteIzquierdo) {
    gfx->drawLine(limiteIzquierdo, yLinea,
                  textoIzquierdo - separacion, yLinea, color);
  }
  if (textoDerecho + separacion < limiteDerecho) {
    gfx->drawLine(textoDerecho + separacion, yLinea,
                  limiteDerecho, yLinea, color);
  }

  gfx->setCursor(textoIzquierdo, baseline);
  gfx->print(texto);
}

// ---------- Bloque exterior/interior ----------
void dibujarBloqueTemperatura(int16_t centroX, float temperatura,
                              int humedad, const char *etiqueta) {
  const Paleta &p = paletaActual();

  // Título
  textoCentrado(etiqueta, centroX, 33, &FreeSans9pt7b, p.acento);

  // Temperatura dominante
  char valor[12];
  snprintf(valor, sizeof(valor), "%.1f", temperatura);
  textoCentrado(valor, centroX - 6, 85, &FreeSansBold24pt7b, p.texto);

  // Símbolo de grados calculado según el ancho real de la fuente
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(&FreeSansBold24pt7b);
  gfx->getTextBounds(valor, 0, 0, &x1, &y1, &w, &h);
  int16_t xGrado = centroX - 6 + (w / 2) + 10;
  gfx->drawCircle(xGrado, 55, 3, p.texto);
  gfx->drawCircle(xGrado, 55, 2, p.texto);

  // Humedad: gota + porcentaje + HUM
  dibujarGota(centroX - 57, 96, p.humedad);

  char porcentaje[8];
  snprintf(porcentaje, sizeof(porcentaje), "%d%%", humedad);
  textoCentrado(porcentaje, centroX + 1, 119, &FreeSansBold12pt7b, p.humedad);

  textoCentrado("HUM", centroX + 48, 119, &FreeSans9pt7b, p.humedad);
}

// ---------- Brújula circular con puntos cardinales ----------
void dibujarBrujula(float grados) {
  const Paleta &p = paletaActual();
  const int16_t cx = COMPASS_X;
  const int16_t cy = 48;
  const int16_t radio = 29;

  // Círculo y marcas
  gfx->drawCircle(cx, cy, radio, p.acento);
  for (int i = 0; i < 32; i++) {
    float aMarca = i * 11.25f * PI / 180.0f;
    bool principal = (i % 4 == 0);
    int16_t r1 = principal ? 23 : 26;
    int16_t r2 = radio - 1;
    int16_t x1 = cx + (int16_t)round(r1 * sin(aMarca));
    int16_t y1 = cy - (int16_t)round(r1 * cos(aMarca));
    int16_t x2 = cx + (int16_t)round(r2 * sin(aMarca));
    int16_t y2 = cy - (int16_t)round(r2 * cos(aMarca));
    gfx->drawLine(x1, y1, x2, y2, p.acento);
  }

  // Cardinales colocados dentro del aro
  textoCentrado("N",  cx,      24, &FreeSansBold9pt7b, p.texto);
  textoCentrado("NE", cx + 19, 36, &FreeSansBold9pt7b, p.texto);
  textoCentrado("E",  cx + 23, 54, &FreeSansBold9pt7b, p.texto);
  textoCentrado("SE", cx + 19, 72, &FreeSansBold9pt7b, p.texto);
  textoCentrado("S",  cx,      83, &FreeSansBold9pt7b, p.texto);
  textoCentrado("SO", cx - 19, 72, &FreeSansBold9pt7b, p.texto);
  textoCentrado("O",  cx - 23, 54, &FreeSansBold9pt7b, p.texto);
  textoCentrado("NO", cx - 19, 36, &FreeSansBold9pt7b, p.texto);

  // Aguja orientada al rumbo
  float a = grados * PI / 180.0f;
  float dx = sin(a);
  float dy = -cos(a);
  float px = -dy;
  float py = dx;

  const float largoPunta = 21.0f;
  const float largoCola = 15.0f;
  const float ancho = 3.8f;

  int16_t puntaX = cx + (int16_t)round(largoPunta * dx);
  int16_t puntaY = cy + (int16_t)round(largoPunta * dy);
  int16_t base1X = cx + (int16_t)round(ancho * px);
  int16_t base1Y = cy + (int16_t)round(ancho * py);
  int16_t base2X = cx - (int16_t)round(ancho * px);
  int16_t base2Y = cy - (int16_t)round(ancho * py);
  gfx->fillTriangle(puntaX, puntaY, base1X, base1Y, base2X, base2Y, p.aguja);

  int16_t colaX = cx - (int16_t)round(largoCola * dx);
  int16_t colaY = cy - (int16_t)round(largoCola * dy);
  int16_t cola1X = cx + (int16_t)round(ancho * px);
  int16_t cola1Y = cy + (int16_t)round(ancho * py);
  int16_t cola2X = cx - (int16_t)round(ancho * px);
  int16_t cola2Y = cy - (int16_t)round(ancho * py);
  gfx->fillTriangle(colaX, colaY, cola1X, cola1Y, cola2X, cola2Y, p.agujaSecundaria);

  // Eje central
  gfx->fillCircle(cx, cy, 4, p.fondo);
  gfx->drawCircle(cx, cy, 4, p.texto);
  gfx->fillCircle(cx, cy, 2, p.agujaSecundaria);

  // Debajo de la brújula: solo los grados
  char gradosTexto[8];
  snprintf(gradosTexto, sizeof(gradosTexto), "%03d", ((int)round(grados) % 360 + 360) % 360);
  char lectura[10];
  snprintf(lectura, sizeof(lectura), "%s", gradosTexto);
  textoCentrado(lectura, cx, 119, &FreeSansBold12pt7b, p.acento);

  // Símbolo de grados
  int16_t bx, by;
  uint16_t bw, bh;
  gfx->setFont(&FreeSansBold12pt7b);
  gfx->getTextBounds(lectura, 0, 0, &bx, &by, &bw, &bh);
  int16_t xGrados = cx + bw / 2 + 4;
  gfx->drawCircle(xGrados, 108, 2, p.acento);
}

// ---------- Interfaz ----------
void dibujarInterfaz() {
  const Paleta &p = paletaActual();
  gfx->fillScreen(p.fondo);

  // Separadores verticales
  gfx->drawLine(142, 19, 142, 123, p.acento);
  gfx->drawLine(286, 19, 286, 123, p.acento);

  // Títulos con línea horizontal interrumpida por el propio texto,
  // como en el diseño de referencia.
  dibujarTituloConLineas("EXTERIOR", EXT_X, 33, 10, 134, p.acento);
  dibujarTituloConLineas("INTERIOR", INT_X, 33, 294, 418, p.acento);

  dibujarBloqueTemperatura(EXT_X, tempExterior, humedadExterior, "EXTERIOR");
  dibujarBloqueTemperatura(INT_X, tempInterior, humedadInterior, "INTERIOR");
  dibujarBrujula(rumbo);
}

// ---------- BLE: conexión ----------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    deviceConnected = true;
    Serial.println("Móvil conectado");
  }

  void onDisconnect(BLEServer *server) override {
    deviceConnected = false;
    Serial.println("Móvil desconectado");
    server->startAdvertising();
  }
};

// ---------- BLE: control del LED RGB ----------
class LEDCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();

    if (value.length() == 3) {
      uint8_t r = (uint8_t)value[0];
      uint8_t g = (uint8_t)value[1];
      uint8_t b = (uint8_t)value[2];
      pixel.setPixelColor(0, pixel.Color(r, g, b));
      pixel.show();
      Serial.printf("LED -> R:%u G:%u B:%u\n", r, g, b);
    } else if (value.length() == 4) {
      uint8_t r = (uint8_t)value[0];
      uint8_t g = (uint8_t)value[1];
      uint8_t b = (uint8_t)value[2];
      uint8_t brightness = (uint8_t)value[3];
      pixel.setBrightness(brightness);
      pixel.setPixelColor(0, pixel.Color(r, g, b));
      pixel.show();
      Serial.printf("LED -> R:%u G:%u B:%u Brillo:%u\n", r, g, b, brightness);
    }
  }
};

// ---------- Pulsador: corto = brillo, largo = tema ----------
const unsigned long DEBOUNCE_MS = 35;
const unsigned long LONG_PRESS_MS = 800;
bool buttonLastReading = HIGH;
bool buttonStableState = HIGH;
unsigned long buttonLastChange = 0;
unsigned long buttonPressedAt = 0;
bool longPressHandled = false;

void aplicarBrilloPantalla() {
  uint8_t brillo = nivelesBrillo[indiceBrillo];
  ledcWrite(TFT_BL, brillo);
  Serial.printf("Brillo pantalla: %u%%\n",
                (unsigned int)round(brillo * 100.0f / 255.0f));
}

void cambiarTema() {
  temaActual = (temaActual == TEMA_DIA) ? TEMA_NOCHE : TEMA_DIA;
  dibujarInterfaz();
  Serial.println(temaActual == TEMA_DIA ? "Tema: DIA" : "Tema: NOCHE");
}

void avanzarBrillo() {
  indiceBrillo = (indiceBrillo + 1) % NUM_NIVELES_BRILLO;
  aplicarBrilloPantalla();
}

void gestionarPulsador() {
  bool lectura = digitalRead(BUTTON_PIN);

  if (lectura != buttonLastReading) {
    buttonLastReading = lectura;
    buttonLastChange = millis();
  }

  if (millis() - buttonLastChange < DEBOUNCE_MS) return;

  if (lectura != buttonStableState) {
    buttonStableState = lectura;

    if (buttonStableState == LOW) {
      buttonPressedAt = millis();
      longPressHandled = false;
    } else {
      // Al soltar: solo se ejecuta la acción corta si no fue pulsación larga.
      if (!longPressHandled) {
        unsigned long duracion = millis() - buttonPressedAt;
        if (duracion < LONG_PRESS_MS) avanzarBrillo();
      }
    }
  }

  // La pulsación larga cambia de tema una sola vez, sin esperar a soltar.
  if (buttonStableState == LOW && !longPressHandled &&
      millis() - buttonPressedAt >= LONG_PRESS_MS) {
    longPressHandled = true;
    cambiarTema();
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Backlight por PWM: GPIO9 debe estar cableado al pin BL/BLK del módulo.
  if (!ledcAttach(TFT_BL, 5000, 8)) {
    Serial.println("Aviso: no se pudo configurar PWM del backlight");
  }

  if (!gfx->begin()) {
    Serial.println("ERROR pantalla");
    while (true) delay(1000);
  }

  pixel.begin();
  pixel.setBrightness(40);
  pixel.setPixelColor(0, 0);
  pixel.show();

  aplicarBrilloPantalla();
  dibujarInterfaz();
  Serial.println("Interfaz lista: tema dia");

  BLEDevice::init("Consola_Techo");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharLED = pService->createCharacteristic(
    CHAR_LED_UUID, BLECharacteristic::PROPERTY_WRITE
  );
  pCharLED->setCallbacks(new LEDCallbacks());

  pCharData = pService->createCharacteristic(
    CHAR_DATA_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharData->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("BLE listo -> Nombre: Consola_Techo");
}

// ---------- Loop ----------
void loop() {
  gestionarPulsador();

  if (millis() - ultimaActualizacion >= 1000) {
    ultimaActualizacion = millis();

    // Datos de demostración. Se sustituirán por sensores cuando estén conectados.
    tempExterior = 18.2 + 0.5 * sin(millis() / 12000.0);
    tempInterior = 24.2 + 0.3 * sin(millis() / 9000.0);
    humedadExterior = 60 + (int)(3 * sin(millis() / 15000.0));
    humedadInterior = 50 + (int)(2 * sin(millis() / 11000.0));

    rumbo += 5.0;
    if (rumbo >= 360.0) rumbo -= 360.0;

    dibujarInterfaz();

    if (deviceConnected) {
      char buffer[64];
      snprintf(buffer, sizeof(buffer), "%.1f,%.1f,%d,%d,%.0f",
               tempExterior, tempInterior,
               humedadExterior, humedadInterior, rumbo);
      pCharData->setValue(buffer);
      pCharData->notify();
    }
  }
}

#include <Arduino_GFX_Library.h>
#include <Adafruit_GFX.h>
#include "fondo_v6.h" // Version con diseño de las circunferencias de las temperaturas más pequeñas que la brujula.
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <math.h>

// ==================================================
// CONSOLA DE TECHO - ESP32-C3 ZERO - v1.4 Arcos dinámicos
// Pantalla NV3007: 428 x 142; zona útil aproximada: y=10..131
// Fondo estático bitmap; tema cambia el color de los datos dinámicos
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
const int16_t EXT_X = 87;
const int16_t COMPASS_X = 214;
const int16_t COMPASS_Y = 71;
const int16_t INT_X = 341;

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
const uint8_t nivelesBrillo[] = {64, 128, 255};
const uint8_t NUM_NIVELES_BRILLO = sizeof(nivelesBrillo) / sizeof(nivelesBrillo[0]);
uint8_t indiceBrillo = 1; // Arranca al 75 %

// Colores RGB565 por tema
struct Paleta {
  uint16_t texto;
  uint16_t humedad;
  uint16_t aguja;
  uint16_t agujaSecundaria;
};

// El bitmap es oscuro en ambos modos. El tema cambia el color de los datos
// superpuestos, sin alterar el fondo aprobado.
const Paleta PALETA_DIA = {
  0xFFFF, // datos blancos
  0xFFFF, // humedad blanca
  0xF800, // punta roja
  0x7BEF  // cola gris
};

const Paleta PALETA_NOCHE = {
  0x07FF, // datos cian
  0x07FF, // humedad cian
  0xF800, // punta roja
  0xBDF7  // cola gris claro
};

const Paleta &paletaActual() {
  return (temaActual == TEMA_DIA) ? PALETA_DIA : PALETA_NOCHE;
}

// ---------- Datos dinámicos sobre el bitmap ----------
// El bitmap ya contiene títulos, círculos, líneas, gota, etiquetas,
// marcas y números cardinales. Aquí solo se dibujan las lecturas variables.
void dibujarTemperatura(int16_t centroX, float temperatura, int16_t ajusteGrado) {
  const Paleta &p = paletaActual();

  char valor[12];
  snprintf(valor, sizeof(valor), "%.1f", temperatura);

  gfx->setFont(&FreeSansBold12pt7b);
  gfx->setTextColor(p.texto);

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(valor, 0, 0, &x1, &y1, &w, &h);

  // Centrar el número junto con el símbolo de grados.
  const int16_t anchoGrado = 7;
  int16_t xTexto = centroX - (int16_t)(w + anchoGrado) / 2;
  gfx->setCursor(xTexto, 72); // posicion de los numeros de la temperatura enY
  gfx->print(valor);

  // Símbolo de grados dibujado para evitar problemas de codificación.
  ///int16_t ajusteGrado = exterior ? 5 : -2;
  int16_t xGrado = xTexto + w + 3 + ajusteGrado;

  gfx->drawCircle(xGrado, 60, 1, p.texto); // 60 es la posicion en Y, 1 es el tipo de circunferencia
  gfx->drawCircle(xGrado, 60, 2, p.texto); // idem
}

void dibujarHumedad(int16_t centroX, int humedad, bool exterior) {
  const Paleta &p = paletaActual();

  char porcentaje[8];
  snprintf(porcentaje, sizeof(porcentaje), "%d%%", humedad);

  gfx->setFont(&FreeSansBold9pt7b);
  gfx->setTextColor(p.humedad);

  // El porcentaje queda al lado de la gota: a la derecha en exterior
  // y a la izquierda en interior, manteniendo la composición simétrica.
  int16_t centroPorcentaje = exterior ? centroX + 10 : centroX + 9; // los dos valores de cenrtoX mueven los numeros de la humedad en X
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(porcentaje, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(centroPorcentaje - (int16_t)w / 2, 99);
  gfx->print(porcentaje);
}

// ---------- Aguja dinámica de la brújula ----------
// El aro, el mapa, las marcas y las letras cardinales están en el bitmap.
void dibujarAguja(float grados) {
  const Paleta &p = paletaActual();
  const int16_t cx = COMPASS_X;
  const int16_t cy = COMPASS_Y;

  float a = grados * PI / 180.0f;
  float dx = sin(a);
  float dy = -cos(a);
  float px = -dy;
  float py = dx;

  const float largoPunta = 33.0f;
  const float largoCola = 23.0f;
  const float ancho = 5.0f;

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
  gfx->fillTriangle(colaX, colaY, cola1X, cola1Y, cola2X, cola2Y,
                    p.agujaSecundaria);

  // Pequeño eje central de la aguja, sin tapar una zona grande del mapa.
  gfx->fillCircle(cx, cy, 2, p.agujaSecundaria);
  gfx->drawCircle(cx, cy, 5, p.texto);
  gfx->drawCircle(cx, cy, 4, p.texto);
}

// ---------- Barras dinámicas de temperatura y humedad ----------
// Coordenadas referidas a la pantalla de 428x142: el bitmap de 428x122
// se dibuja en y=10, por lo que los centros de los círculos quedan en y=71.
const int16_t CENTRO_ARCO_Y = 71;
const int16_t RADIO_ARCO_TEMP = 57;  // pista exterior: temperatura
const int16_t RADIO_ARCO_HUM  = 49;  // pista interior: humedad
const int16_t GROSOR_ARCO = 4;
const int16_t RADIO_BOLITA = 4;

uint16_t colorRGB565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint8_t mezclarCanal(uint8_t a, uint8_t b, float t) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return (uint8_t)roundf(a + (b - a) * t);
}

// Degradado con amarillo saturado para que no se vea blanco en la TFT pequeña.
uint16_t colorTemperatura(float proporcion) {
  if (proporcion < 0.0f) proporcion = 0.0f;
  if (proporcion > 1.0f) proporcion = 1.0f;

  // Azul intenso -> celeste -> amarillo vivo -> naranja -> rojo.
  const uint8_t azul[3]     = {0, 105, 255};
  const uint8_t celeste[3]  = {0, 205, 255};
  const uint8_t amarillo[3] = {255, 230, 0};
  const uint8_t naranja[3]  = {255, 105, 0};
  const uint8_t rojo[3]     = {245, 20, 8};

  const uint8_t *a;
  const uint8_t *b;
  float t;
  if (proporcion < 0.25f) {
    a = azul; b = celeste; t = proporcion / 0.25f;
  } else if (proporcion < 0.52f) {
    a = celeste; b = amarillo; t = (proporcion - 0.25f) / 0.27f;
  } else if (proporcion < 0.76f) {
    a = amarillo; b = naranja; t = (proporcion - 0.52f) / 0.24f;
  } else {
    a = naranja; b = rojo; t = (proporcion - 0.76f) / 0.24f;
  }
  return colorRGB565(mezclarCanal(a[0], b[0], t),
                     mezclarCanal(a[1], b[1], t),
                     mezclarCanal(a[2], b[2], t));
}

// Humedad: azul oscuro a baja intensidad, azul vivo al subir el porcentaje.
uint16_t colorHumedad(int humedad) {
  if (humedad < 0) humedad = 0;
  if (humedad > 100) humedad = 100;
  float intensidad = 0.10f + 0.90f * (humedad / 100.0f);
  uint8_t r = 0;
  uint8_t g = (uint8_t)roundf(45.0f * intensidad);
  uint8_t b = (uint8_t)roundf(255.0f * intensidad);
  return colorRGB565(r, g, b);
}

void dibujarArco(int16_t centroX, int16_t centroY, int16_t radio,
                 float anguloInicio, float barrido, uint16_t color,
                 bool dibujarBolitaFinal, bool degradadoTemperatura = false) {
  if (barrido <= 0.0f) return;
  if (barrido > 360.0f) barrido = 360.0f;

  // Segmentos cortos para aproximar el arco; nunca se dibuja una vuelta completa
  // salvo que el valor alcance el 100 % de su escala.
  const float paso = 2.0f;
  float anguloAnterior = anguloInicio;
  int16_t xAnterior = centroX + (int16_t)roundf(radio * cosf(anguloAnterior * PI / 180.0f));
  int16_t yAnterior = centroY + (int16_t)roundf(radio * sinf(anguloAnterior * PI / 180.0f));
  for (float recorrido = paso; recorrido < barrido; recorrido += paso) {
    float angulo = anguloInicio + recorrido;
    int16_t x = centroX + (int16_t)roundf(radio * cosf(angulo * PI / 180.0f));
    int16_t y = centroY + (int16_t)roundf(radio * sinf(angulo * PI / 180.0f));
    uint16_t colorSegmento = degradadoTemperatura
      ? colorTemperatura(recorrido / 360.0f) : color;
    gfx->drawLine(xAnterior, yAnterior, x, y, colorSegmento);
    // El grosor se completa con círculos pequeños a lo largo del trazo.
    gfx->fillCircle(x, y, GROSOR_ARCO / 2, colorSegmento);
    xAnterior = x;
    yAnterior = y;
    anguloAnterior = angulo;
  }

  float anguloFinal = anguloInicio + barrido;
  int16_t xFinal = centroX + (int16_t)roundf(radio * cosf(anguloFinal * PI / 180.0f));
  int16_t yFinal = centroY + (int16_t)roundf(radio * sinf(anguloFinal * PI / 180.0f));
  uint16_t colorFinal = degradadoTemperatura
    ? colorTemperatura(barrido / 360.0f) : color;
  gfx->drawLine(xAnterior, yAnterior, xFinal, yFinal, colorFinal);
  gfx->fillCircle(xFinal, yFinal, GROSOR_ARCO / 2, colorFinal);

  if (dibujarBolitaFinal) {
    gfx->fillCircle(xFinal, yFinal, RADIO_BOLITA, colorFinal);
  }
}

void dibujarBarrasIndicadores() {
  // Temperatura: escala -10..50 °C; inicio a las 9 h y avance horario.
  float proporcionTempExt = (tempExterior + 10.0f) / 60.0f;
  float proporcionTempInt = (tempInterior + 10.0f) / 60.0f;
  if (proporcionTempExt < 0.0f) proporcionTempExt = 0.0f;
  if (proporcionTempExt > 1.0f) proporcionTempExt = 1.0f;
  if (proporcionTempInt < 0.0f) proporcionTempInt = 0.0f;
  if (proporcionTempInt > 1.0f) proporcionTempInt = 1.0f;

  // Humedad: escala 0..100 %; inicio a las 3 h y avance horario.
  int humExt = constrain(humedadExterior, 0, 100);
  int humInt = constrain(humedadInterior, 0, 100);

  // Las dos magnitudes usan radios distintos para que sus trazos nunca se unan,
  // incluso cuando sus recorridos angulares se solapan.
  dibujarArco(EXT_X, CENTRO_ARCO_Y, RADIO_ARCO_TEMP, 180.0f,
              proporcionTempExt * 360.0f, colorTemperatura(proporcionTempExt), true, true);
  dibujarArco(INT_X, CENTRO_ARCO_Y, RADIO_ARCO_TEMP, 180.0f,
              proporcionTempInt * 360.0f, colorTemperatura(proporcionTempInt), true, true);

  dibujarArco(EXT_X, CENTRO_ARCO_Y, RADIO_ARCO_HUM, 0.0f,
              humExt * 3.6f, colorHumedad(humExt), true);
  dibujarArco(INT_X, CENTRO_ARCO_Y, RADIO_ARCO_HUM, 0.0f,
              humInt * 3.6f, colorHumedad(humInt), true);
}

// ---------- Interfaz ----------
// Se repinta el bitmap antes de dibujar los valores para borrar las lecturas
// anteriores y evitar restos al cambiar temperatura, humedad o rumbo.
void dibujarInterfaz() {
  gfx->draw16bitRGBBitmap(
    0, 10,
    fondo,
    FONDO_WIDTH,
    FONDO_HEIGHT
  );

  dibujarTemperatura(EXT_X, tempExterior, 4); // el 0 es la posicion de º de exterior en x
  dibujarTemperatura(INT_X, tempInterior, 2); // el 0 es la posicion de º de interior en x

  dibujarHumedad(EXT_X, humedadExterior, true);
  dibujarHumedad(INT_X, humedadInterior, false);

  dibujarBarrasIndicadores();
  dibujarAguja(rumbo);
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

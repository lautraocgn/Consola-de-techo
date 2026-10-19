#include <Arduino_GFX_Library.h>
#include <Adafruit_GFX.h>
#include "fondo_v6.h" // Version con las tres circunferencias del mismo tamaño
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
// CONSOLA DE TECHO - ESP32-C3 ZERO - v2.0 Humedad con degradado de opacidad y refresco incremental
// Pantalla NV3007: 428 x 142; zona útil aproximada: y=10..131
// Fondo estático; refresco por regiones solo cuando cambian los datos
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


// ---------- Fondo por tema ----------
// Noche conserva el bitmap aprobado sin cambios. En modo día se aclaran
// únicamente los píxeles casi negros a un azul marino discreto, manteniendo
// intactos los textos, círculos, brújula y detalles de color del bitmap.
const uint16_t FONDO_DIA_OSCURO = 0x0843; // RGB565: azul marino moderadamente aclarado

uint16_t colorFondoEn(int16_t x, int16_t y) {
  if (x < 0 || x >= FONDO_WIDTH || y < 10 || y >= 10 + FONDO_HEIGHT) return 0x0000;
  uint16_t c = fondo[(int32_t)(y - 10) * FONDO_WIDTH + x];
  if (temaActual == TEMA_DIA) {
    uint8_t r = ((c >> 11) & 0x1F) * 255 / 31;
    uint8_t g = ((c >> 5) & 0x3F) * 255 / 63;
    uint8_t b = (c & 0x1F) * 255 / 31;
    // Solo sustituye tonos prácticamente negros; no altera elementos claros.
    if (r <= 8 && g <= 12 && b <= 32) return FONDO_DIA_OSCURO;
  }
  return c;
}

void dibujarBitmapFondoPorTema() {
  uint16_t fila[FONDO_WIDTH];
  for (int16_t y = 0; y < FONDO_HEIGHT; ++y) {
    for (int16_t x = 0; x < FONDO_WIDTH; ++x) {
      uint16_t c = fondo[(int32_t)y * FONDO_WIDTH + x];
      if (temaActual == TEMA_DIA) {
        uint8_t r = ((c >> 11) & 0x1F) * 255 / 31;
        uint8_t g = ((c >> 5) & 0x3F) * 255 / 63;
        uint8_t b = (c & 0x1F) * 255 / 31;
        if (r <= 8 && g <= 12 && b <= 32) c = FONDO_DIA_OSCURO;
      }
      fila[x] = c;
    }
    gfx->draw16bitRGBBitmap(0, y + 10, fila, FONDO_WIDTH, 1);
  }
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
const int16_t RADIO_ARCO_TEMP = 57;  // ambas magnitudes comparten la misma circunferencia
const int16_t RADIO_ARCO_HUM  = RADIO_ARCO_TEMP;
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

// Color base de humedad; la intensidad visual se mezcla con el bitmap de fondo
// para simular transparencia real en la pantalla RGB565.
uint16_t colorHumedad(int humedad) {
  if (humedad < 0) humedad = 0;
  if (humedad > 100) humedad = 100;
  return colorRGB565(0, 45, 255);
}

uint16_t mezclarConFondo(uint16_t color, int16_t x, int16_t y, int humedad) {
  if (humedad <= 0) return colorFondoEn(x, y); // seguridad; al 0 % no se dibuja el arco
  if (humedad > 100) humedad = 100;
  int16_t bx = x;
  int16_t by = y - 10; // bitmap 428x122 mostrado desde y=10
  uint16_t bg = 0;
  if (bx >= 0 && bx < FONDO_WIDTH && by >= 0 && by < FONDO_HEIGHT) {
    bg = colorFondoEn(bx, y);
  }
  float alpha = humedad / 100.0f;
  uint8_t cr = ((color >> 11) & 0x1F) * 255 / 31;
  uint8_t cg = ((color >> 5) & 0x3F) * 255 / 63;
  uint8_t cb = (color & 0x1F) * 255 / 31;
  uint8_t br = ((bg >> 11) & 0x1F) * 255 / 31;
  uint8_t bgc = ((bg >> 5) & 0x3F) * 255 / 63;
  uint8_t bb = (bg & 0x1F) * 255 / 31;
  return colorRGB565(
    (uint8_t)roundf(br + (cr - br) * alpha),
    (uint8_t)roundf(bgc + (cg - bgc) * alpha),
    (uint8_t)roundf(bb + (cb - bb) * alpha));
}

void dibujarPixelMezclado(int16_t x, int16_t y, uint16_t color, int humedad) {
  if (x < 0 || x >= 428 || y < 0 || y >= 142 || humedad <= 0) return;
  gfx->drawPixel(x, y, mezclarConFondo(color, x, y, humedad));
}

void dibujarSegmentoMezclado(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                             uint16_t color, int humedad) {
  int16_t dx = x1 - x0;
  int16_t dy = y1 - y0;
  int pasos = max(abs(dx), abs(dy));
  if (pasos == 0) pasos = 1;
  for (int i = 0; i <= pasos; ++i) {
    int16_t x = x0 + (int32_t)dx * i / pasos;
    int16_t y = y0 + (int32_t)dy * i / pasos;
    for (int16_t oy = -GROSOR_ARCO / 2; oy <= GROSOR_ARCO / 2; ++oy) {
      for (int16_t ox = -GROSOR_ARCO / 2; ox <= GROSOR_ARCO / 2; ++ox) {
        dibujarPixelMezclado(x + ox, y + oy, color, humedad);
      }
    }
  }
}

void dibujarBolitaMezclada(int16_t cx, int16_t cy, uint16_t color, int humedad) {
  for (int16_t y = -RADIO_BOLITA; y <= RADIO_BOLITA; ++y) {
    for (int16_t x = -RADIO_BOLITA; x <= RADIO_BOLITA; ++x) {
      if (x * x + y * y <= RADIO_BOLITA * RADIO_BOLITA) {
        dibujarPixelMezclado(cx + x, cy + y, color, humedad);
      }
    }
  }
}

void dibujarArco(int16_t centroX, int16_t centroY, int16_t radio,
                 float anguloInicio, float barrido, uint16_t color,
                 bool dibujarBolitaFinal, bool degradadoTemperatura = false, int humedadAlpha = -1) {
  if (barrido <= 0.0f) return;
  if (barrido > 180.0f) barrido = 180.0f;

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
      ? colorTemperatura(recorrido / 180.0f) : color;
    if (humedadAlpha >= 0) {
      // La opacidad depende de la posición angular, no del porcentaje actual.
      // Así, cada píxel conserva su nivel aunque la barra se alargue o acorte.
      int alphaPosicion = constrain((int)roundf((recorrido / 180.0f) * 100.0f), 0, 100);
      dibujarSegmentoMezclado(xAnterior, yAnterior, x, y, colorSegmento, alphaPosicion);
    } else {
      gfx->drawLine(xAnterior, yAnterior, x, y, colorSegmento);
      gfx->fillCircle(x, y, GROSOR_ARCO / 2, colorSegmento);
    }
    xAnterior = x;
    yAnterior = y;
    anguloAnterior = angulo;
  }

  float anguloFinal = anguloInicio + barrido;
  int16_t xFinal = centroX + (int16_t)roundf(radio * cosf(anguloFinal * PI / 180.0f));
  int16_t yFinal = centroY + (int16_t)roundf(radio * sinf(anguloFinal * PI / 180.0f));
  uint16_t colorFinal = degradadoTemperatura
    ? colorTemperatura(barrido / 180.0f) : color;
  if (humedadAlpha >= 0) {
    int alphaFinal = constrain((int)roundf((barrido / 180.0f) * 100.0f), 0, 100);
    dibujarSegmentoMezclado(xAnterior, yAnterior, xFinal, yFinal, colorFinal, alphaFinal);
  } else {
    gfx->drawLine(xAnterior, yAnterior, xFinal, yFinal, colorFinal);
    gfx->fillCircle(xFinal, yFinal, GROSOR_ARCO / 2, colorFinal);
  }

  if (dibujarBolitaFinal) {
    // Se conserva exactamente el radio aprobado de la bolita terminal.
    if (humedadAlpha >= 0) {
      int alphaFinal = constrain((int)roundf((barrido / 180.0f) * 100.0f), 0, 100);
      dibujarBolitaMezclada(xFinal, yFinal, color, alphaFinal);
    } else gfx->fillCircle(xFinal, yFinal, RADIO_BOLITA, colorFinal);
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

  // Temperatura: semicírculo superior, desde las 9 h hacia las 3 h.
  // La proporción -10..50 °C determina tanto el barrido (0..180°) como el color.
  dibujarArco(EXT_X, CENTRO_ARCO_Y, RADIO_ARCO_TEMP, 180.0f,
              proporcionTempExt * 180.0f, colorTemperatura(proporcionTempExt), true, true);
  dibujarArco(INT_X, CENTRO_ARCO_Y, RADIO_ARCO_TEMP, 180.0f,
              proporcionTempInt * 180.0f, colorTemperatura(proporcionTempInt), true, true);

  // Humedad: semicírculo inferior, desde las 3 h hacia las 9 h.
  // 0 % no dibuja nada; la opacidad se degrada por posición angular, estable al cambiar la longitud.
  if (humExt > 0) dibujarArco(EXT_X, CENTRO_ARCO_Y, RADIO_ARCO_HUM, 0.0f,
              humExt * 1.8f, colorHumedad(humExt), true, false, humExt);
  if (humInt > 0) dibujarArco(INT_X, CENTRO_ARCO_Y, RADIO_ARCO_HUM, 0.0f,
              humInt * 1.8f, colorHumedad(humInt), true, false, humInt);
}

// ---------- Refresco independiente de cada dato dinámico ----------
// Se restaura desde el bitmap únicamente la huella del arco anterior y el
// rectángulo del texto que cambia. Así, actualizar temperatura no repinta
// humedad, ni actualizar humedad repinta temperatura.
void restaurarPixelFondo(int16_t x, int16_t y) {
  if (x < 0 || x >= FONDO_WIDTH || y < 10 || y >= 132) return;
  gfx->drawPixel(x, y, colorFondoEn(x, y));
}

void restaurarDiscoFondo(int16_t cx, int16_t cy, int16_t radio) {
  for (int16_t oy = -radio; oy <= radio; ++oy) {
    for (int16_t ox = -radio; ox <= radio; ++ox) {
      if (ox * ox + oy * oy <= radio * radio) restaurarPixelFondo(cx + ox, cy + oy);
    }
  }
}

void restaurarHuellaArco(int16_t centroX, float barrido, bool humedad) {
  if (barrido <= 0.0f) return;
  if (barrido > 180.0f) barrido = 180.0f;
  const float paso = 2.0f;
  int16_t radio = humedad ? RADIO_ARCO_HUM : RADIO_ARCO_TEMP;
  float inicio = humedad ? 0.0f : 180.0f;
  float recorrido = 0.0f;
  while (recorrido <= barrido) {
    float a = (inicio + recorrido) * PI / 180.0f;
    int16_t x = centroX + (int16_t)roundf(radio * cosf(a));
    int16_t y = CENTRO_ARCO_Y + (int16_t)roundf(radio * sinf(a));
    restaurarDiscoFondo(x, y, humedad ? RADIO_BOLITA : GROSOR_ARCO / 2);
    recorrido += paso;
  }
  // Limpia también con el radio aprobado la posición terminal anterior.
  float af = (inicio + barrido) * PI / 180.0f;
  int16_t xf = centroX + (int16_t)roundf(radio * cosf(af));
  int16_t yf = CENTRO_ARCO_Y + (int16_t)roundf(radio * sinf(af));
  restaurarDiscoFondo(xf, yf, RADIO_BOLITA);
}

void restaurarTexto(int16_t centroX, bool esHumedad) {
  int16_t x = centroX - (esHumedad ? 30 : 34);
  int16_t y = esHumedad ? 86 : 48;
  int16_t w = esHumedad ? 60 : 68;
  int16_t h = esHumedad ? 20 : 31;
  for (int16_t py = y; py < y + h; ++py)
    for (int16_t px = x; px < x + w; ++px)
      restaurarPixelFondo(px, py);
}

float proporcionTemperatura(float temperatura) {
  return constrain((temperatura + 10.0f) / 60.0f, 0.0f, 1.0f);
}

void actualizarTemperatura(int16_t centroX, float anterior, float actual, bool exterior) {
  restaurarHuellaArco(centroX, proporcionTemperatura(anterior) * 180.0f, false);
  restaurarTexto(centroX, false);
  dibujarTemperatura(centroX, actual, exterior ? 4 : 2);
  float proporcion = proporcionTemperatura(actual);
  dibujarArco(centroX, CENTRO_ARCO_Y, RADIO_ARCO_TEMP, 180.0f,
              proporcion * 180.0f, colorTemperatura(proporcion), true, true);
}

// Dibuja solo un tramo de la barra de humedad. La opacidad es fija por
// posición angular (0..100 % a lo largo de los 180 grados), no por lectura.
void dibujarTramoHumedad(int16_t centroX, float desde, float hasta, bool bolitaFinal) {
  if (hasta <= desde || hasta <= 0.0f) return;
  if (desde < 0.0f) desde = 0.0f;
  if (hasta > 180.0f) hasta = 180.0f;
  const float paso = 1.0f;
  float a = desde;
  int16_t xAnterior = centroX + (int16_t)roundf(RADIO_ARCO_HUM * cosf(a * PI / 180.0f));
  int16_t yAnterior = CENTRO_ARCO_Y + (int16_t)roundf(RADIO_ARCO_HUM * sinf(a * PI / 180.0f));
  for (float recorrido = desde + paso; recorrido < hasta; recorrido += paso) {
    float rad = recorrido * PI / 180.0f;
    int16_t x = centroX + (int16_t)roundf(RADIO_ARCO_HUM * cosf(rad));
    int16_t y = CENTRO_ARCO_Y + (int16_t)roundf(RADIO_ARCO_HUM * sinf(rad));
    int alpha = constrain((int)roundf(recorrido / 180.0f * 100.0f), 0, 100);
    dibujarSegmentoMezclado(xAnterior, yAnterior, x, y, colorHumedad(100), alpha);
    xAnterior = x; yAnterior = y;
  }
  float radFinal = hasta * PI / 180.0f;
  int16_t xFinal = centroX + (int16_t)roundf(RADIO_ARCO_HUM * cosf(radFinal));
  int16_t yFinal = CENTRO_ARCO_Y + (int16_t)roundf(RADIO_ARCO_HUM * sinf(radFinal));
  int alphaFinal = constrain((int)roundf(hasta / 180.0f * 100.0f), 0, 100);
  dibujarSegmentoMezclado(xAnterior, yAnterior, xFinal, yFinal, colorHumedad(100), alphaFinal);
  if (bolitaFinal) dibujarBolitaMezclada(xFinal, yFinal, colorHumedad(100), alphaFinal);
}

void restaurarTramoHumedad(int16_t centroX, float desde, float hasta) {
  if (hasta <= desde) return;
  if (desde < 0.0f) desde = 0.0f;
  if (hasta > 180.0f) hasta = 180.0f;
  for (float recorrido = desde; recorrido <= hasta; recorrido += 1.0f) {
    float rad = recorrido * PI / 180.0f;
    int16_t x = centroX + (int16_t)roundf(RADIO_ARCO_HUM * cosf(rad));
    int16_t y = CENTRO_ARCO_Y + (int16_t)roundf(RADIO_ARCO_HUM * sinf(rad));
    restaurarDiscoFondo(x, y, RADIO_BOLITA);
  }
}

void actualizarHumedad(int16_t centroX, int anterior, int actual, bool exterior) {
  int humAnterior = constrain(anterior, 0, 100);
  int humActual = constrain(actual, 0, 100);
  float barridoAnterior = humAnterior * 1.8f;
  float barridoActual = humActual * 1.8f;

  // El texto se actualiza aparte; la barra solo toca la zona del extremo móvil.
  restaurarTexto(centroX, true);
  dibujarHumedad(centroX, actual, exterior);

  if (barridoActual < barridoAnterior) {
    // Al acortar, borrar solo la cola que desaparece y la bolita antigua.
    restaurarTramoHumedad(centroX, max(0.0f, barridoActual - 2.0f), barridoAnterior + 1.0f);
    if (barridoActual > 0.0f) dibujarTramoHumedad(centroX, max(0.0f, barridoActual - 2.0f), barridoActual, true);
  } else if (barridoActual > barridoAnterior) {
    // Al alargar, borrar la bolita antigua, conservar el tramo existente y añadir solo la extensión.
    if (barridoAnterior > 0.0f) {
      float radAnt = barridoAnterior * PI / 180.0f;
      int16_t xa = centroX + (int16_t)roundf(RADIO_ARCO_HUM * cosf(radAnt));
      int16_t ya = CENTRO_ARCO_Y + (int16_t)roundf(RADIO_ARCO_HUM * sinf(radAnt));
      restaurarDiscoFondo(xa, ya, RADIO_BOLITA);
      dibujarTramoHumedad(centroX, max(0.0f, barridoAnterior - 2.0f), barridoAnterior, false);
    }
    dibujarTramoHumedad(centroX, max(0.0f, barridoAnterior - 1.0f), barridoActual, true);
  } else if (barridoActual > 0.0f) {
    // Por seguridad, si el porcentaje coincide no se repinta la barra.
  }
}

void restaurarRectanguloFondo(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (x < 0) { w += x; x = 0; }
  if (x + w > 428) w = 428 - x;
  if (y < 10) { h -= (10 - y); y = 10; }
  if (y + h > 132) h = 132 - y;
  if (w <= 0 || h <= 0) return;
  for (int16_t fila = y; fila < y + h; ++fila) {
    uint16_t filaColor[428];
    for (int16_t px = 0; px < w; ++px) filaColor[px] = colorFondoEn(x + px, fila);
    gfx->draw16bitRGBBitmap(x, fila, filaColor, w, 1);
  }
}

void actualizarBrujula() {
  restaurarRectanguloFondo(COMPASS_X - 39, COMPASS_Y - 39, 78, 78);
  dibujarAguja(rumbo);
}

float tempExteriorPintada = NAN;
float tempInteriorPintada = NAN;
int humedadExteriorPintada = -1;
int humedadInteriorPintada = -1;
float rumboPintado = NAN;

bool distintoFloat(float a, float b, float epsilon = 0.05f) {
  return isnan(a) || fabsf(a - b) >= epsilon;
}

void dibujarInterfaz() {
  gfx->fillScreen(0x0000);
  dibujarBitmapFondoPorTema();
  // Al iniciar o cambiar de tema se dibujan todos los datos.
  float pe = proporcionTemperatura(tempExterior);
  float pi = proporcionTemperatura(tempInterior);
  dibujarTemperatura(EXT_X, tempExterior, 4);
  dibujarTemperatura(INT_X, tempInterior, 2);
  dibujarHumedad(EXT_X, humedadExterior, true);
  dibujarHumedad(INT_X, humedadInterior, false);
  dibujarArco(EXT_X, CENTRO_ARCO_Y, RADIO_ARCO_TEMP, 180.0f, pe * 180.0f,
              colorTemperatura(pe), true, true);
  dibujarArco(INT_X, CENTRO_ARCO_Y, RADIO_ARCO_TEMP, 180.0f, pi * 180.0f,
              colorTemperatura(pi), true, true);
  int he = constrain(humedadExterior, 0, 100), hi = constrain(humedadInterior, 0, 100);
  if (he > 0) dibujarArco(EXT_X, CENTRO_ARCO_Y, RADIO_ARCO_HUM, 0.0f, he * 1.8f,
                          colorHumedad(he), true, false, he);
  if (hi > 0) dibujarArco(INT_X, CENTRO_ARCO_Y, RADIO_ARCO_HUM, 0.0f, hi * 1.8f,
                          colorHumedad(hi), true, false, hi);
  dibujarAguja(rumbo);
  tempExteriorPintada = tempExterior;
  tempInteriorPintada = tempInterior;
  humedadExteriorPintada = humedadExterior;
  humedadInteriorPintada = humedadInterior;
  rumboPintado = rumbo;
}

void actualizarInterfazDinamica() {
  if (distintoFloat(tempExterior, tempExteriorPintada)) {
    actualizarTemperatura(EXT_X, tempExteriorPintada, tempExterior, true);
    tempExteriorPintada = tempExterior;
  }
  if (distintoFloat(tempInterior, tempInteriorPintada)) {
    actualizarTemperatura(INT_X, tempInteriorPintada, tempInterior, false);
    tempInteriorPintada = tempInterior;
  }
  if (humedadExterior != humedadExteriorPintada) {
    actualizarHumedad(EXT_X, humedadExteriorPintada, humedadExterior, true);
    humedadExteriorPintada = humedadExterior;
  }
  if (humedadInterior != humedadInteriorPintada) {
    actualizarHumedad(INT_X, humedadInteriorPintada, humedadInterior, false);
    humedadInteriorPintada = humedadInterior;
  }
  if (distintoFloat(rumbo, rumboPintado, 0.5f)) {
    actualizarBrujula();
    rumboPintado = rumbo;
  }
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

    // Datos de demostración: al conectar sensores, sustituir estas asignaciones por lecturas reales.
    // La actualización gráfica ya compara las lecturas y omite zonas sin cambios.
    tempExterior = 18.2 + 0.5 * sin(millis() / 12000.0);
    tempInterior = 24.2 + 0.3 * sin(millis() / 9000.0);
    humedadExterior = 60 + (int)(3 * sin(millis() / 15000.0));
    humedadInterior = 50 + (int)(2 * sin(millis() / 11000.0));

    rumbo += 5.0;
    if (rumbo >= 360.0) rumbo -= 360.0;

    actualizarInterfazDinamica();

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
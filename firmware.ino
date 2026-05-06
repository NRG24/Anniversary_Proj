#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <FS.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <JPEGDEC.h>
#include <PNGdec.h>

#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TFT_SCLK 18
#define TFT_MOSI 23
#define TFT_BL   15

const char* apName = "AnniversaryFrame";
const char* apPass = "anniversary";

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);
JPEGDEC jpeg;
PNG png;

String currentImage = "";
bool imageDirty = false;

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Anniversary Frame</title>
<style>
body{font-family:Arial,sans-serif;background:#111;color:#eee;max-width:760px;margin:0 auto;padding:24px}
h1{margin-top:0}
.card{background:#1b1b1b;padding:18px;border-radius:14px;margin-bottom:18px}
input,button,select{font-size:16px;padding:10px;border-radius:10px;border:none}
button{background:#4f8cff;color:white;cursor:pointer}
button:hover{background:#3b76dc}
a{color:#8ab4ff;text-decoration:none}
ul{padding-left:18px}
li{margin:10px 0}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.small{opacity:.75;font-size:14px}
</style>
</head>
<body>
<h1>Anniversary Frame</h1>
<div class="card">
<div class="small">Connect to Wi-Fi:</div>
<div><b>SSID:</b> AnniversaryFrame</div>
<div><b>Password:</b> anniversary</div>
<div class="small">Then open <a href="http://192.168.4.1">http://192.168.4.1</a></div>
</div>

<div class="card">
<form method="POST" action="/upload" enctype="multipart/form-data">
<div class="row">
<input type="file" name="image" accept=".jpg,.jpeg,.png,.bmp,image/jpeg,image/png,image/bmp" required>
<button type="submit">Upload</button>
</div>
</form>
</div>

<div class="card">
<form method="POST" action="/show">
<div class="row">
<select name="file">
%OPTIONS%
</select>
<button type="submit">Display</button>
</div>
</form>
</div>

<div class="card">
<form method="POST" action="/delete">
<div class="row">
<select name="file">
%OPTIONS%
</select>
<button type="submit">Delete</button>
</div>
</form>
</div>

<div class="card">
<form method="POST" action="/clear">
<button type="submit">Clear Screen</button>
</form>
</div>

<div class="card">
<h3>Stored Images</h3>
%LIST%
</div>
</body>
</html>
)rawliteral";

String contentTypeFor(const String& path) {
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".bmp")) return "image/bmp";
  if (path.endsWith(".html")) return "text/html";
  return "application/octet-stream";
}

bool isImageFile(const String& name) {
  String s = name;
  s.toLowerCase();
  return s.endsWith(".jpg") || s.endsWith(".jpeg") || s.endsWith(".png") || s.endsWith(".bmp");
}

String imageOptions() {
  String out;
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (isImageFile(name)) {
      String selected = (name == currentImage) ? " selected" : "";
      out += "<option value=\"" + name + "\"" + selected + ">" + name + "</option>";
    }
    file = root.openNextFile();
  }
  if (out.length() == 0) out = "<option value=\"\">No images</option>";
  return out;
}

String imageList() {
  String out = "<ul>";
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  bool any = false;
  while (file) {
    String name = String(file.name());
    if (isImageFile(name)) {
      any = true;
      out += "<li><a href=\"/file?name=" + name + "\">" + name + "</a>";
      if (name == currentImage) out += " <b>(showing)</b>";
      out += "</li>";
    }
    file = root.openNextFile();
  }
  if (!any) out += "<li>No images uploaded</li>";
  out += "</ul>";
  return out;
}

String renderPage() {
  String page = INDEX_HTML;
  page.replace("%OPTIONS%", imageOptions());
  page.replace("%LIST%", imageList());
  return page;
}

void drawRGB565ScaledCentered(uint16_t *lineBuf, int srcW, int srcH, int y, int outY, int drawW, int drawH, int xOffset, int yOffset) {
  int destY = outY + yOffset;
  if (destY < 0 || destY >= tft.height()) return;
  static uint16_t scaledLine[160];
  for (int x = 0; x < drawW; x++) {
    int srcX = (x * srcW) / drawW;
    scaledLine[x + xOffset] = lineBuf[srcX];
  }
  tft.drawRGBBitmap(xOffset, destY, &scaledLine[xOffset], drawW, 1);
}

int jpegDrawCallback(JPEGDRAW *pDraw) {
  int screenW = tft.width();
  int screenH = tft.height();
  int imgW = jpeg.getWidth();
  int imgH = jpeg.getHeight();

  float sx = (float)screenW / imgW;
  float sy = (float)screenH / imgH;
  float scale = sx < sy ? sx : sy;
  int drawW = imgW * scale;
  int drawH = imgH * scale;
  int xOffset = (screenW - drawW) / 2;
  int yOffset = (screenH - drawH) / 2;

  int srcY = pDraw->y;
  int blockH = pDraw->iHeight;
  uint16_t *pixels = (uint16_t*)pDraw->pPixels;

  for (int row = 0; row < blockH; row++) {
    int absoluteY = srcY + row;
    int mappedY0 = (absoluteY * drawH) / imgH;
    int mappedY1 = ((absoluteY + 1) * drawH) / imgH;
    if (mappedY1 <= mappedY0) mappedY1 = mappedY0 + 1;
    for (int yy = mappedY0; yy < mappedY1; yy++) {
      int destY = yy + yOffset;
      if (destY < 0 || destY >= screenH) continue;
      static uint16_t line[160];
      for (int x = 0; x < drawW; x++) {
        int srcX = (x * imgW) / drawW;
        if (srcX >= pDraw->x && srcX < (pDraw->x + pDraw->iWidth)) {
          int localX = srcX - pDraw->x;
          line[x] = pixels[row * pDraw->iWidth + localX];
        }
      }
      tft.drawRGBBitmap(xOffset, destY, line, drawW, 1);
    }
  }
  return 1;
}

void showBMP(const char* path) {
  File f = SPIFFS.open(path, "r");
  if (!f) return;

  if (read16(f) != 0x4D42) {
    f.close();
    return;
  }

  read32(f);
  read32(f);
  uint32_t imageOffset = read32(f);
  uint32_t headerSize = read32(f);
  int32_t bmpWidth = read32(f);
  int32_t bmpHeight = read32(f);
  if (read16(f) != 1) {
    f.close();
    return;
  }

  uint16_t depth = read16(f);
  uint32_t compression = read32(f);
  if (compression != 0 || (depth != 24 && depth != 16)) {
    f.close();
    return;
  }

  bool flip = true;
  if (bmpHeight < 0) {
    bmpHeight = -bmpHeight;
    flip = false;
  }

  int screenW = tft.width();
  int screenH = tft.height();
  float sx = (float)screenW / bmpWidth;
  float sy = (float)screenH / bmpHeight;
  float scale = sx < sy ? sx : sy;
  int drawW = bmpWidth * scale;
  int drawH = bmpHeight * scale;
  int xOffset = (screenW - drawW) / 2;
  int yOffset = (screenH - drawH) / 2;

  uint32_t rowSize = ((bmpWidth * depth / 8) + 3) & ~3;
  uint8_t sdbuffer[3 * 20];
  uint16_t line[160];

  tft.fillScreen(ST77XX_BLACK);

  for (int y = 0; y < drawH; y++) {
    int srcY = (y * bmpHeight) / drawH;
    uint32_t pos = imageOffset + (flip ? (bmpHeight - 1 - srcY) : srcY) * rowSize;
    f.seek(pos);

    int buffidx = sizeof(sdbuffer);

    for (int x = 0; x < drawW; x++) {
      int srcX = (x * bmpWidth) / drawW;
      uint32_t pixelPos = pos + srcX * (depth / 8);
      if (f.position() != pixelPos) {
        f.seek(pixelPos);
        buffidx = sizeof(sdbuffer);
      }

      uint8_t b, g, r;
      if (depth == 24) {
        if (buffidx >= sizeof(sdbuffer)) {
          f.read(sdbuffer, sizeof(sdbuffer));
          buffidx = 0;
        }
        b = sdbuffer[buffidx++];
        if (buffidx >= sizeof(sdbuffer)) {
          f.read(sdbuffer, sizeof(sdbuffer));
          buffidx = 0;
        }
        g = sdbuffer[buffidx++];
        if (buffidx >= sizeof(sdbuffer)) {
          f.read(sdbuffer, sizeof(sdbuffer));
          buffidx = 0;
        }
        r = sdbuffer[buffidx++];
        line[x] = tft.color565(r, g, b);
      } else {
        uint16_t px;
        f.read((uint8_t*)&px, 2);
        uint8_t rr = ((px >> 11) & 0x1F) << 3;
        uint8_t gg = ((px >> 5) & 0x3F) << 2;
        uint8_t bb = (px & 0x1F) << 3;
        line[x] = tft.color565(rr, gg, bb);
      }
    }

    tft.drawRGBBitmap(xOffset, y + yOffset, line, drawW, 1);
  }

  f.close();
}

uint16_t read16(File &f) {
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  return result;
}

uint32_t read32(File &f) {
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read();
  return result;
}

void displayImage(const String& path) {
  if (!SPIFFS.exists(path)) return;

  tft.fillScreen(ST77XX_BLACK);

  String lower = path;
  lower.toLowerCase();

  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
    if (jpeg.open(path.c_str(), jpegDrawCallback)) {
      jpeg.decode(0, 0, JPEG_SCALE_FULL);
      jpeg.close();
    }
  } else if (lower.endsWith(".png")) {
    int rc = png.open(path.c_str(), [](PNGDRAW *pDraw) {
      static uint16_t lineBuffer[160];
      int screenW = tft.width();
      int screenH = tft.height();
      int imgW = png.getWidth();
      int imgH = png.getHeight();

      float sx = (float)screenW / imgW;
      float sy = (float)screenH / imgH;
      float scale = sx < sy ? sx : sy;
      int drawW = imgW * scale;
      int drawH = imgH * scale;
      int xOffset = (screenW - drawW) / 2;
      int yOffset = (screenH - drawH) / 2;

      png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xffffffff);

      int mappedY0 = (pDraw->y * drawH) / imgH;
      int mappedY1 = ((pDraw->y + 1) * drawH) / imgH;
      if (mappedY1 <= mappedY0) mappedY1 = mappedY0 + 1;

      static uint16_t scaledLine[160];
      for (int x = 0; x < drawW; x++) {
        int srcX = (x * imgW) / drawW;
        scaledLine[x] = lineBuffer[srcX];
      }

      for (int yy = mappedY0; yy < mappedY1; yy++) {
        int destY = yy + yOffset;
        if (destY >= 0 && destY < screenH) {
          tft.drawRGBBitmap(xOffset, destY, scaledLine, drawW, 1);
        }
      }
    });
    if (rc == PNG_SUCCESS) {
      png.decode(nullptr, 0);
      png.close();
    }
  } else if (lower.endsWith(".bmp")) {
    showBMP(path.c_str());
  }

  currentImage = path;
}

void handleRoot() {
  server.send(200, "text/html", renderPage());
}

void handleShow() {
  if (!server.hasArg("file")) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  String file = server.arg("file");
  if (file.length() == 0 || !SPIFFS.exists(file)) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  displayImage(file);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDelete() {
  if (server.hasArg("file")) {
    String file = server.arg("file");
    if (SPIFFS.exists(file)) {
      SPIFFS.remove(file);
      if (file == currentImage) {
        currentImage = "";
        tft.fillScreen(ST77XX_BLACK);
      }
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleClear() {
  currentImage = "";
  tft.fillScreen(ST77XX_BLACK);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleFileView() {
  if (!server.hasArg("name")) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  String file = server.arg("name");
  if (!SPIFFS.exists(file)) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  File f = SPIFFS.open(file, "r");
  server.streamFile(f, contentTypeFor(file));
  f.close();
}

File uploadFile;

void handleUploadDone() {
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    filename.replace(" ", "_");
    String lower = filename;
    lower.toLowerCase();
    if (!(lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png") || lower.endsWith(".bmp"))) return;
    uploadFile = SPIFFS.open(filename, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    filename.replace(" ", "_");
    displayImage(filename);
  }
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/show", HTTP_POST, handleShow);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/file", HTTP_GET, handleFileView);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.begin();
}

void setupDisplay() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
}

void setupWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName, apPass);
}

void setup() {
  Serial.begin(115200);
  SPIFFS.begin(true);
  setupDisplay();
  setupWiFi();
  setupRoutes();

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextWrap(true);
  tft.setCursor(8, 30);
  tft.setTextSize(1);
  tft.println("Anniversary Frame");
  tft.println("");
  tft.println("Connect WiFi:");
  tft.println("AnniversaryFrame");
  tft.println("");
  tft.println("Open:");
  tft.println("192.168.4.1");
}

void loop() {
  server.handleClient();
}
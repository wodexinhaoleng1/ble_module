/**
 * VG6328A 蓝牙串口透传 (优化版)
 */

#include <Arduino.h>
#include <HardwareSerial.h>

#define GPIO_STATUS     2       // 模块 Pin4 LED 状态输出 (INPUT)
#define GPIO_LED        8       // 模块 Pin3 外部红色LED (OUTPUT, 低电平亮) ⚠️确保不是GPIO6!
#define BLE_RX          4       // 模块 Pin5 TXD -> ESP32 RX
#define BLE_TX          5       // 模块 Pin6 RXD -> ESP32 TX
#define BLE_BAUD        115200
#define BLE_DEVICE_NAME "XLCX_test"   // ← 修改此处设置蓝牙名称（最多20字节）

HardwareSerial BleSerial(1);

static bool g_connected = false;

// ─── 优化后的 AT 发送（带动态超时等待）───
static String sendAT(const char *cmd, uint32_t timeoutMs = 600) {
    while (BleSerial.available()) BleSerial.read(); // 清空缓存
    
    BleSerial.print(cmd); 
    BleSerial.print("\r\n");
    
    String resp = "";
    uint32_t startMs = millis();
    
    // 动态等待：直到超时或者检测到结束标志(比如"OK"或"ERR")
    while (millis() - startMs < timeoutMs) {
        while (BleSerial.available()) {
            resp += (char)BleSerial.read();
            startMs = millis(); // 收到字符，刷新超时判定
        }
        if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
            break; 
        }
        delay(5); // 稍微释放CPU
    }
    
    String printResp = resp;
    printResp.trim();
    Serial.printf("  %-10s << %s\r\n", cmd, printResp.length() ? printResp.c_str() : "(无响应)");
    return resp;
}

// ─── 发送 AT 并以十六进制打印响应（用于二进制返回值）───
static void sendATHex(const char *cmd, uint32_t timeoutMs = 600) {
    while (BleSerial.available()) BleSerial.read();
    BleSerial.print(cmd);
    BleSerial.print("\r\n");
    String hex = "";
    uint32_t t = millis();
    while (millis() - t < timeoutMs) {
        while (BleSerial.available()) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", (uint8_t)BleSerial.read());
            hex += buf;
            t = millis();
        }
        delay(5);
    }
    hex.trim();
    Serial.printf("  %-10s << %s\r\n", cmd, hex.length() ? hex.c_str() : "(无响应)");
}

// ─── 查询 + 配置模块 ──────────────────────────────────
static void queryModule() {
    Serial.println("\r\n=== VG6328A 模块信息 ===");

    String r = sendAT("AT+ENAT", 500);
    if (r.indexOf("OK") < 0) {
        Serial.println("  [FAIL] 请检查: 接线 / 供电 3.3V / 波特率 115200");
        Serial.println("========================\r\n");
        return;
    }

    sendAT("AT+VERS");          // 固件版本（文本）
    sendATHex("AT+FUID");       // Flash UID（二进制→hex）
    sendAT("AT+LEGN");          // 当前 BLE 名称（文本）
    sendATHex("AT+LEGA");       // MAC 地址（二进制→hex）

    // AT+CONN 返回单字节: 0x04=未连接, 0x10=BLE已连接
    while (BleSerial.available()) BleSerial.read();
    BleSerial.print("AT+CONN\r\n");
    delay(400);
    uint8_t connByte = 0; bool gotConn = false;
    while (BleSerial.available()) { connByte = (uint8_t)BleSerial.read(); gotConn = true; }
    if (gotConn)
        Serial.printf("  AT+CONN    << 0x%02X (%s)\r\n", connByte,
                      connByte == 0x10 ? "BLE已连接" : connByte == 0x04 ? "未连接" : "未知");
    else
        Serial.println("  AT+CONN    << (无响应)");

    // ─── 配置：设置名称 + 开启广播 + 复位生效 ───
    Serial.println("---");
    sendAT("AT+LENA" BLE_DEVICE_NAME);  // 设置 BLE 名称（掉电保存）
    sendAT("AT+LEON");                  // 开启 BLE 广播（掉电保存）
    Serial.printf("  复位中，新名称 \"%s\" 生效后自动广播...\r\n", BLE_DEVICE_NAME);
    BleSerial.print("AT+REST\r\n");
    delay(1500);
    Serial.println("========================");
    Serial.printf("  手机搜索: \"%s\"\r\n", BLE_DEVICE_NAME);
    Serial.println("  Service UUID: 0xFFE0");
    Serial.println("  Char 0xFFE0  Write Without Response  APP->UART");
    Serial.println("  Char 0xFFE0  Notify                  UART->APP\r\n");
}

void setup() {
    Serial.begin(115200);
    // GPIO_STATUS: 状态LED输出（低电平亮）
    // 注意：若 VG6328A Pin4 也驱动此引脚，请确认为开漏输出或已隔离
    pinMode(GPIO_STATUS, OUTPUT);
    digitalWrite(GPIO_STATUS, HIGH);  // 上电默认灭
    pinMode(GPIO_LED, OUTPUT);
    digitalWrite(GPIO_LED, HIGH);   // 上电默认灭

    Serial.println("\r\n--- VG6328A 透传程序启动 ---");
    for (int i = 3; i > 0; i--) {
        Serial.printf("  %d 秒后读取模块信息...\r\n", i);
        delay(1000);
    }

    BleSerial.begin(BLE_BAUD, SERIAL_8N1, BLE_RX, BLE_TX);
    delay(200);

    queryModule();
}

void loop() {
    uint32_t now = millis();

    // ── 1. 每 500ms 短暂切 INPUT 采样连接状态，再恢复 OUTPUT ──
    static uint32_t lastCheck = 0;
    if (now - lastCheck >= 500) {
        lastCheck = now;
        pinMode(GPIO_STATUS, INPUT);
        delayMicroseconds(100);             // 等待电平稳定
        bool nowConn = (digitalRead(GPIO_STATUS) == LOW);
        pinMode(GPIO_STATUS, OUTPUT);       // 恢复输出
        if (nowConn != g_connected) {
            g_connected = nowConn;
            Serial.println(g_connected ? "[BLE] 已连接" : "[BLE] 已断开，广播中...");
        }
    }

    // ── 2. 状态LED (GPIO_STATUS): 未连接→500ms闪烁，已连接→常亮 ──
    if (g_connected) {
        digitalWrite(GPIO_STATUS, LOW);                        // 常亮
    } else {
        digitalWrite(GPIO_STATUS, (now % 1000) < 500 ? LOW : HIGH); // 闪烁
    }

    // ── 3. 数据透传 ──
    bool active = false;

    // 控制台 → BLE (完全非阻塞)
    while (Serial.available()) {
        BleSerial.write(Serial.read());
        active = true;
    }

    // BLE → 控制台（30ms 断帧非阻塞流）
    static String rxBuf = "";
    static uint32_t lastRx = 0;
    while (BleSerial.available()) {
        rxBuf += (char)BleSerial.read();
        lastRx = now;
        active = true;
    }
    if (rxBuf.length() > 0 && (now - lastRx >= 30)) {
        Serial.print(rxBuf);
        rxBuf = "";
    }

    // ── 4. 数据LED (GPIO_LED): 有数据活动亮 50ms，否则灭 ──
    static uint32_t ledOff = 0;
    if (active) {
        digitalWrite(GPIO_LED, LOW);   // 亮
        ledOff = now + 50;
    } else if (now >= ledOff) {
        digitalWrite(GPIO_LED, HIGH);  // 灭
    }
}
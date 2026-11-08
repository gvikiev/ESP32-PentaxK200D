# Интеграция Pentax K200D с ESP32 по USB: Протоколы и Архитектура Web UI

## 1. Обзор фотокамеры Pentax K200D

Цифровая зеркальная фотокамера **Pentax K200D** обладает широким набором аппаратных и программных функций, объединяющих особенности любительских и полупрофессиональных моделей своего поколения:

* **Матрица:** 10,2-мегапиксельный CCD-сенсор формата APS-C (23,5 × 15,7 мм).
* **Корпус и защита:** Пыле- и влагозащищённый корпус (60 уплотнителей), система защиты матрицы от пыли (Dust Removal System & Dust Alert).
* **Стабилизация:** Встроенная система оптической стабилизации матрицы (Shake Reduction).
* **Автофокусировка:** 11-точечная система фазовой фокусировки (SAFOX VIII).
* **Форматы файлов:** `.JPEG`, фирменный RAW (`.PEF`) и универсальный открытый RAW (`.DNG`).
* **Дисплеи:** Основной цветной ЖК-экран 2.7" (230 000 точек) и вспомогательный монохромный верхний дисплей.
* **Питание:** 4 элемента формата АА / батарейная ручка `D-BG3`.

---

## 2. Специфика дистанционного управления по USB (SCSI vs PTP)

Управление съемкой Pentax K200D через USB устроено иначе, чем у большинства современных цифровых камер:

```
                  ┌─────────────────────────────────────────┐
                  │          Меню USB Pentax K200D          │
                  └────────────────────┬────────────────────┘
                                       │
                    ┌──────────────────┴──────────────────┐
                    ▼                                     ▼
           [ PC Drive (MSC) ]                           [ PTP ]
   (USB Mass Storage Class: 0x08)            (Still Image Class: 0x06)
                    │                                     │
   ┌────────────────┴────────────────┐          ┌─────────┴─────────┐
   ▼                                 ▼          ▼                   ▼
Управление съемкой             Скачивание    Скачивание          Дистанционное
(SCSI over USB BOT)           из RAM-буфера  с SD-карты          управление
   [Доступно]                  [Доступно]   [Доступно]         [НЕ поддерживается]
```

### 2.1. Vendor-Specific SCSI/USB Протокол (Режим PC Drive / MSC)

В режимах `PC Drive` камеры K10D, K20D, K100D Super и K200D работают через **проприетарные SCSI-команды**, инкапсулированные в транспорт **USB Mass Storage Class (MSC / BOT — Bulk-Only Transport)**.

* **Транспорт:** USB Mass Storage Device (`bInterfaceClass = 0x08`).
* **Низкоуровневый интерфейс:** Сигналы передаются через блоки дескрипторов команд SCSI (Command Descriptor Block, CDB) с вендорскими кодами операций (`Vendor-Unique OpCodes`).

#### Ключевые команды и этапы взаимодействия (по спецификации `pkTriggerCord` / `libgphoto2`):

1. **Передача аргументов (CDB `0x4F`):** Задание параметров (ISO, выдержка, диафрагма и др.).
2. **Исполнение команды (CDB `0x24`):** Отправка группы и номера команды:
   * `[00 05]` — Инициализация подключения.
   * `[00 00]` — Спуск затвора (Shutter Release).
   * `[00 01]` — Полунажатие / Автофокусировка (Half-press / SAFOX VIII).
3. **Опрос статуса (`Get Status`, CDB `0x26`):** Циклический опрос состояния готовности затвора и процесса обработки кадра.
4. **Считывание буфера (`Buffer Read`, CDB `0x04` / `0x49`):** 
   * При удаленном управлении кадр сохраняется во внутренний RAM-буфер камеры.
   * ПО запрашивает размер данных буфера (`[02 00]`) и вычитывает готовый файл (`.PEF` / `.DNG` / `.JPG`) блоками на хост.

#### Ограничения протокола на K200D:
* **Отсутствие Live View и видео:** CCD-матрица физически не поддерживает режим непрерывного визирования по экрану и запись видео.
* **Задержки обработки:** Запись кадра CCD-сенсором и процессором требует контроля таймаутов (1–3 секунды перед выгрузкой).

---

### 2.2. Режим PTP (Picture Transfer Protocol)

* **Поддержка функций:** Камера отвечает на стандартные команды PTP (`GetDeviceInfo`, `GetObject`, `GetThumb` и т. д.) для просмотра и скачивания снимков с SD-карты.
* **Ограничение:** PTP Vendor Extensions для удаленного управления съемкой (Tethered Shooting) в K200D **отсутствуют**. Спуск затвора и смена настроек по PTP не поддерживаются.

---

## 3. Реализация на ESP32 (ESP-IDF v5.x)

### 3.1. Структура USB BOT CBW пакета (SCSI Encapsulation)

```c
#include <stdint.h>
#include <string.h>

// Структура USB Bulk-Only Transport Command Block Wrapper
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;          // "USBC" (0x43425355)
    uint32_t dCBWTag;                // Уникальный ID транзакции
    uint32_t dCBWDataTransferLength; // Длина ожидаемых данных
    uint8_t  bmCBWFlags;             // 0x80 (Read) или 0x00 (Write)
    uint8_t  bCBWLUN;               // 0x00
    uint8_t  bCBWCBLength;          // Длина SCSI CDB (12 байт)
    uint8_t  CBWCB[16];              // SCSI Command Descriptor Block
} usb_msc_cbw_t;

// Формирование Pentax Vendor Command (CDB 0x24)
void build_pentax_cmd(usb_msc_cbw_t *cbw, uint8_t group, uint8_t cmd_code) {
    memset(cbw, 0, sizeof(usb_msc_cbw_t));
    cbw->dCBWSignature = 0x43425355;
    cbw->dCBWTag = 0x01;
    cbw->dCBWDataTransferLength = 0;
    cbw->bmCBWFlags = 0x00;
    cbw->bCBWCBLength = 12;

    cbw->CBWCB[0] = 0xF0; 
    cbw->CBWCB[1] = 0x24; // OpCode: Execute Command
    cbw->CBWCB[2] = group;
    cbw->CBWCB[3] = cmd_code;
}
```

---

### 3.2. Автоматическое определение режима USB при подключении

При изменении настроек в меню камеры происходят сброс шины USB и повторная инициализация (Re-enumeration). ESP32 вычитывает `bInterfaceClass` из дескриптора конфигурации:

```c
#include "usb/usb_host.h"

typedef enum {
    CAM_MODE_DISCONNECTED = 0,
    CAM_MODE_MSC_CONTROL, // Режим "PC Drive" (Mass Storage Class 0x08)
    CAM_MODE_PTP_BROWSE   // Режим "PTP" (Still Image Class 0x06)
} camera_mode_t;

volatile camera_mode_t current_camera_mode = CAM_MODE_DISCONNECTED;

void usb_host_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg) {
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        usb_device_handle_t dev_hdl;
        usb_host_device_open(client_hdl, event_msg->new_dev.address, &dev_hdl);

        const usb_config_desc_t *config_desc;
        usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
        
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)usb_parse_interface_descriptor(config_desc, 0, 0, NULL);

        if (intf->bInterfaceClass == 0x08) { 
            // Режим PC Drive (MSC): Загрузка драйвера управления по SCSI
            current_camera_mode = CAM_MODE_MSC_CONTROL;
            init_pentax_scsi_driver(dev_hdl);
        } else if (intf->bInterfaceClass == 0x06) { 
            // Режим PTP: Загрузка драйвера чтения файлов SD-карты
            current_camera_mode = CAM_MODE_PTP_BROWSE;
            init_ptp_driver(dev_hdl);
        }

        // Уведомление подключенных Web-клиентов
        broadcast_camera_status();

    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_DEREGISTERED) {
        current_camera_mode = CAM_MODE_DISCONNECTED;
        broadcast_camera_status();
    }
}
```

---

## 4. Динамический Web UI на базе WebSockets

### 4.1. Схема состояний UI

| Состояние | Режим камеры | Функционал Web UI |
| :--- | :--- | :--- |
| **Disconnected** | `NONE` | Экран ожидания подключения камеры по USB. |
| **PC Drive** | `MSC` | **Пульт ДУ:** кнопка спуска, сменщики ISO / Av / Tv / RAW, индикатор буфера. |
| **PTP** | `PTP` | **Галерея SD-карты:** просмотр списков файлов, скачивание кадров. |

---

### 4.2. Серверная часть ESP32 (C++ / WebSockets)

```cpp
void broadcast_camera_status() {
    String json = "{\"type\":\"STATUS_UPDATE\",\"mode\":";
    if (current_camera_mode == CAM_MODE_MSC_CONTROL) {
        json += "\"MSC\"}";
    } else if (current_camera_mode == CAM_MODE_PTP_BROWSE) {
        json += "\"PTP\"}";
    } else {
        json += "\"NONE\"}";
    }
    ws.textAll(json);
}
```

---

### 4.3. Клиентская часть Frontend (JS / WebSockets)

```javascript
const ws = new WebSocket(`ws://${location.host}/ws`);

ws.onmessage = (event) => {
    const data = JSON.parse(event.data);
    if (data.type === 'STATUS_UPDATE') {
        switchModeUI(data.mode);
    }
};

function switchModeUI(mode) {
    document.getElementById('view-disconnected').style.display = 'none';
    document.getElementById('view-msc-control').style.display = 'none';
    document.getElementById('view-ptp-gallery').style.display = 'none';

    if (mode === 'MSC') {
        document.getElementById('view-msc-control').style.display = 'block';
        initRemoteControlPanel();
    } else if (mode === 'PTP') {
        document.getElementById('view-ptp-gallery').style.display = 'block';
        loadSdCardGallery();
    } else {
        document.getElementById('view-disconnected').style.display = 'block';
    }
}
```

---

## 5. Сводная таблица взаимодействия

| Функция | Необходимый режим USB | Способ реализации |
| :--- | :--- | :--- |
| **Спуск затвора и съемка** | PC Drive (MSC) | SCSI CDB `0x24` через USB BOT |
| **Настройка ISO, Av, Tv** | PC Drive (MSC) | SCSI CDB `0x4F` (Set Param) & `0x24` |
| **Выгрузка свежего кадра** | PC Drive (MSC) | SCSI CDB `0x04` / `0x49` из RAM-буфера |
| **Просмотр старых кадров с SD** | PTP | PTP Class Driver (`GetObject`, `GetThumb`) |
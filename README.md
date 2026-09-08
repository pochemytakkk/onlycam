# OnlyCam

Телефон как веб-камера для Windows. OnlyCam ставит в систему собственное
устройство камеры **OnlyCam** (DirectShow), поэтому его видно в OBS, Discord,
Zoom, Teams и в «Камере» Windows — OBS для работы не нужен.

## Как это устроено

```
телефон  ──JPEG──►  приложение OnlyCam  ──BGR24 в shared memory──►  DirectShow-фильтр  ──►  OBS / Discord / Zoom
```

- **Источник 1 — браузер телефона.** Приложение поднимает локальный HTTPS-сервер,
  показывает QR-код; телефон открывает страницу, отдаёт камеру через
  `getUserMedia` и шлёт кадры по WebSocket. Ставить ничего не нужно.
- **Источник 2 — приложение OnlyCam на iPhone.** Приложение забирает MJPEG-поток
  `http://<ip-телефона>:8080/stream`.
- **Виртуальная камера** — COM-DLL `OnlyCamFilter*.dll`, зарегистрированная в
  категории `CLSID_VideoInputDeviceCategory`. Кадры она читает из именованной
  проекции памяти `Local\OnlyCamFrameBuffer`.

## Установка (Windows)

1. Скачай `OnlyCam-Setup.exe` из артефактов сборки
   (Actions → «Build Windows app» → артефакт `OnlyCam-Setup`).
2. Запусти установщик от администратора — он ставит приложение и регистрирует
   виртуальную камеру (x64 и x86, чтобы 32-битные программы тоже её видели).
3. Открой OnlyCam, подключи телефон и нажми «Запустить камеру».
4. В OBS/Discord выбери устройство **OnlyCam**.

Портативный вариант: артефакт `OnlyCam-portable`, внутри `OnlyCam.exe`; в самом
приложении нажми «Установить виртуальную камеру» — оно вызовет `regsvr32`
с запросом прав администратора.

## Телефон через браузер

1. Телефон и компьютер в одной Wi-Fi-сети.
2. В приложении выбери «Телефон через браузер (QR-код)» и нажми «Запустить камеру».
3. Отсканируй QR-код камерой телефона и открой ссылку `https://<ip>:8443/`.
4. Браузер предупредит о самоподписанном сертификате — это нормально, сертификат
   выписывается локально на твоём компьютере (Safari: «Подробнее» → «Посетить
   этот веб-сайт»; Chrome: «Дополнительные» → «Перейти на сайт»).
5. Разреши доступ к камере и нажми «Стримить». Вкладку не сворачивай — iOS
   останавливает камеру в фоне.

## Приложение OnlyCam на iPhone

1. Собери IPA (Actions → «Build iPhone IPA») и поставь через Sideloadly.
2. Запусти приложение — оно покажет адрес вида `http://192.168.0.10:8080/stream`.
3. В Windows-приложении выбери «Приложение OnlyCam на iPhone», впиши адрес и
   нажми «Запустить камеру».

## Разработка

```powershell
cd onlycam\windows\app
python -m venv .venv
.venv\Scripts\pip install -r requirements-dev.txt
.venv\Scripts\python -m onlycam     # запуск GUI
.venv\Scripts\python -m pytest tests
.venv\Scripts\ruff check .
```

Сборка DirectShow-фильтра (нужен Visual Studio 2022 + CMake):

```powershell
cmake -S onlycam\windows\filter -B build\x64 -A x64
cmake --build build\x64 --config Release
regsvr32 build\x64\Release\OnlyCamFilter.dll
```

Структура:

| Путь | Что это |
| --- | --- |
| `onlycam/windows/filter` | DirectShow source filter (C++) |
| `onlycam/windows/app` | Приложение с GUI, HTTPS/WebSocket-сервер, пайплайн кадров |
| `onlycam/windows/installer` | Inno Setup installer |
| `onlycam/ios` | Приложение для iPhone (MJPEG-сервер) |

## Известные ограничения

- 32-битные программы видят камеру только если зарегистрирована x86-версия DLL
  (установщик делает это сам).
- Кадры идут в RGB24; при 1080p30 это ~180 МБ/с через shared memory — локально
  это дёшево, но CPU на JPEG-декодирование расходуется.
- Windows Hello / приложения на Media Foundation Frame Server (например,
  «Камера» в Windows 11) DirectShow-камеры не показывают.

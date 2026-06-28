# Server & Client — Remote Control via ImGui

C++ приложение с ImGui GUI: сервер удалённо управляет клиентом через релейный сервер на OnRender.

## Возможности

| Функция | Описание |
|---|---|
| **Browse Files** | Просмотр файловой системы клиента (навигация по каталогам) |
| **System Info** | Информация о системе клиента (ОС, CPU, память, IP, разрешение) |
| **View Screen** | Захват экрана клиента (JPEG, авто-обновление) |
| **Execute** | Выполнение команд cmd на клиенте |

## Архитектура

```
┌──────────┐    TCP    ┌─────────────┐    TCP    ┌──────────┐
│  Сервер  │◄────────►│    Релей    │◄────────►│  Клиент  │
│  (ImGui) │           │ (OnRender)  │           │  (ImGui) │
└──────────┘           └─────────────┘           └──────────┘
     Windows              Linux/Docker              Windows
```

- **Сервер** — GUI на ImGui (DX11 + Win32), отправляет команды клиенту
- **Клиент** — GUI на ImGui, авто-подключается к релею, выполняет команды
- **Релей** — TCP-маршрутизатор, разворачивается на OnRender через Docker

## Сборка (Visual Studio + CMake)

### Требования
- Visual Studio 2022 (с компонентом C++ CMake Tools)
- Windows SDK (входит в VS)

### Шаги

1. Откройте папку проекта в Visual Studio (File → Open → Folder)
2. VS автоматически определит `CMakeLists.txt` и настроит проект
3. Выберите цель сборки: `server.exe` или `client.exe`
4. Build → Build All (или Ctrl+Shift+B)

### Альтернатива: командная строка

```bat
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

Результат: `build\Release\server.exe`, `build\Release\client.exe`, `build\Release\relay.exe`

## Деплой релея на OnRender

### Вариант 1: C++ релей (Docker)

1. Зарегистрируйтесь на [render.com](https://render.com)
2. Создайте новый Web Service → подключите Git-репозиторий
3. Render автоматически найдёт `render.yaml` и `Dockerfile`
4. Или вручную:
   - **Type:** Web Service
   - **Runtime:** Docker
   - **Plan:** Free
   - **Dockerfile Path:** `./Dockerfile`

### Вариант 2: Python релей (проще)

Замените `dockerfilePath` в `render.yaml`:
```yaml
dockerfilePath: ./Dockerfile.python
```

### Локальный релей (для тестирования)

```bat
relay.exe 10000
:: или
python relay\relay.py 10000
```

## Настройка

### Адрес релея в клиенте

После деплоя на OnRender вы получите URL вида:
```
https://your-relay-name.onrender.com
```

Измените адрес в **client/main.cpp** перед сборкой:
```cpp
#define DEFAULT_RELAY_HOST "your-relay-name.onrender.com"
#define DEFAULT_RELAY_PORT 443
```

Или введите адрес вручную в GUI клиента после запуска.

### Адрес релея в сервере

Введите адрес в поле "Relay" в GUI сервера.

## Использование

1. **Запустите релей** (локально или на OnRender)
2. **Запустите client.exe** — он автоматически подключится к релею
3. **Запустите server.exe** — подключитесь к релею
4. Выберите клиента из списка
5. Используйте вкладки:
   - **Browse Files** — введите путь (напр. `C:\`), нажмите Browse
   - **System Info** — нажмите "Get System Info"
   - **View Screen** — нажмите "Capture" или включите Auto-refresh
   - **Execute** — введите команду cmd, нажмите Execute

## Структура проекта

```
├── CMakeLists.txt          # Сборка (CMake)
├── Dockerfile              # C++ релей для OnRender
├── Dockerfile.python       # Python релей для OnRender
├── render.yaml             # Конфиг OnRender
├── common/
│   ├── protocol.h          # Бинарный протокол (header + payload)
│   ├── network.h           # Кроссплатформенный TCP (WinSock2/POSIX)
│   ├── network.cpp         # Реализация сети
│   ├── imgui_setup.h       # ImGui + DX11 + Win32 boilerplate
│   └── imgui_setup.cpp      # Реализация ImGui setup
├── server/
│   └── main.cpp            # GUI сервера (BrowseFiles, SystemInfo, ViewScreen, Execute)
├── client/
│   └── main.cpp            # GUI клиента (автоподключение, выполнение команд)
├── relay/
│   ├── main.cpp            # C++ релей-сервер
│   └── relay.py            # Python релей-сервер (альтернатива)
└── imgui-1.92.8/           # Библиотека ImGui
```

## Протокол

```
┌──────────┬──────────┬──────────┬────────────────────────┐
│ payloadSz│  msgType │  cmdType │  payload bytes ...     │
│  uint32  │  uint8   │  uint8   │  (payloadSz bytes)     │
└──────────┴──────────┴──────────┴────────────────────────┘
```

- `MSG_REGISTER` (1) — регистрация клиента/сервера на релее
- `MSG_CLIENT_LIST` (2) — список клиентов (релей → сервер)
- `MSG_COMMAND` (3) — команда (сервер → клиент)
- `MSG_RESPONSE` (4) — ответ (клиент → сервер)

## Устранение проблем

| Проблема | Решение |
|---|---|
| Клиент не подключается | Проверьте адрес релея в GUI |
| Нет клиентов в списке | Убедитесь что клиент запущен и подключён |
| View Screen не работает | Проверьте что экран не заблокирован |
| OnRender не работает | Попробуйте Python Dockerfile или ngrok |
| Ошибка сборки | Убедитесь что Visual Studio имеет C++ tools |

# Развёртывание rendezvous на VDS

Bootstrap-сервер `nyx-rendezvous` нужен для **связи через интернет**: без него работают только LAN (mDNS) и прямой `--peer`.

Клиенты **не отправляют** сообщения через rendezvous — только регистрируют `invite_token → UDP endpoint` для hole punch.

## Требования

| Параметр | Значение |
|----------|----------|
| Протокол | **UDP** |
| Порт по умолчанию | **3478** |
| RAM | ≥ 64 MB |
| CPU | 1 vCPU достаточно для сотен клиентов |

## Быстрый старт (Linux)

```bash
# На VDS
git clone <repo> && cd game.ru
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target nyx-rendezvous

sudo ufw allow 3478/udp
./build/nyx-rendezvous --bind=0.0.0.0:3478 --rate-limit=120
```

## systemd

Файл `/etc/systemd/system/nyx-rendezvous.service`:

```ini
[Unit]
Description=Nyx rendezvous bootstrap
After=network-online.target

[Service]
Type=simple
ExecStart=/opt/Nyx/nyx-rendezvous --bind=0.0.0.0:3478 --rate-limit=240
Restart=always
RestartSec=3
User=Nyx
AmbientCapabilities=

[Install]
WantedBy=multi-user.target
```

```bash
sudo useradd -r -s /usr/sbin/nologin Nyx
sudo install -m755 build/nyx-rendezvous /opt/Nyx/
sudo systemctl daemon-reload
sudo systemctl enable --now nyx-rendezvous
sudo systemctl status nyx-rendezvous
```

## Docker

```dockerfile
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
COPY nyx-rendezvous /usr/local/bin/
EXPOSE 3478/udp
ENTRYPOINT ["/usr/local/bin/nyx-rendezvous", "--bind=0.0.0.0:3478"]
```

```bash
docker build -t nyx-rendezvous -f deploy/rendezvous/Dockerfile .
docker run -d --name Nyx-rv -p 3478:3478/udp --restart unless-stopped nyx-rendezvous
```

## Настройка клиентов

### GUI (nyx-app)

1. **Настройки → Сеть → Обнаружение peer**
2. Режим: **Авто** или **Только Интернет**
3. Rendezvous: `your-vds.example.com:3478` (несколько через запятую)
4. **Проверить первый** → **Сохранить**

Конфиг: `%APPDATA%/Nyx/network.json` (Windows) или `~/.config/nyx/network.json`.

### CLI (nyx-node)

```powershell
nyx-node listen --rendezvous your-vds.example.com:3478
nyx-node connect --token <64hex> --rendezvous your-vds.example.com:3478
```

Несколько серверов (failover lookup):

```powershell
nyx-node listen --rendezvous rv1.example.com:3478,rv2.example.com:3478
```

## Облако (security group)

| Провайдер | Правило |
|-----------|---------|
| AWS | Inbound UDP 3478 from 0.0.0.0/0 |
| Hetzner | Firewall UDP 3478 |
| Yandex Cloud | SG: ingress UDP 3478 |

Исходящий UDP клиентам **не блокируйте** — нужен для hole punch.

## Мониторинг

- Логи: stdout / `%APPDATA%/Nyx/logs/` на клиентах
- Rendezvous: `journalctl -u nyx-rendezvous -f`
- Проверка с клиента: кнопка «Проверить» в GUI или `nyx-node` lookup

## Безопасность оператора

Rendezvous **видит** token ↔ IP:port, **не видит** содержимое чата (шифруется Noise после P2P).

Рекомендации:

- Rate limit (`--rate-limit=120`)
- Отдельный VDS под rendezvous
- Не публиковать rendezvous в открытых списках без rate limit
- План: HMAC register (см. `docs/PROTOCOL_EVOLUTION.md`)

## Скрипт установки

```bash
bash scripts/deploy-rendezvous-linux.sh --install /opt/Nyx
```

См. также [ARCHITECTURE.md](ARCHITECTURE.md), [protocol.md](protocol.md).

# ── Python WebSocket relay Dockerfile ───────────────────────────────
FROM python:3.12-slim
WORKDIR /app
RUN pip install --no-cache-dir aiohttp
COPY relay/relay.py .
COPY requirements.txt .
EXPOSE 10000
CMD ["python", "-u", "relay.py"]

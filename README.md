# MyImGuiMetalProject

Обобщённая обёртка над графическими API: один цикл кадра (`FastRenderer::run_loop()`) поверх
абстракций из `render_pipeline/`, с бэкендами Metal (`render_metal/`) и Vulkan (`render_vulkan/`).

Подробное описание пайплайна — создание поверхности, командные буферы, синхронизация, модель
биндингов и паритет шейдеров — в [PIPELINE.ru.md](PIPELINE.ru.md).

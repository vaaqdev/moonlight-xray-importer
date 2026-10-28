Moonlight 6.2.0 + Xray - Portable para macOS Catalina 10.15 (Intel)

Descomprime TODO el ZIP con Utilidad de Compresion y abre Moonlight.app.
No requiere instalador ni copiar la app a Aplicaciones. Importa tu perfil
JSON desde los ajustes y reinicia cuando te lo pida.

Build legacy: Qt 6.3.2, librerias nativas recompiladas para 10.15,
Xray v24.12.31 compilado con Go 1.22.12. No usa Vulkan/libplacebo;
conserva VideoToolbox/Metal para video. Xray y Qt son versiones antiguas:
no contienen todas las correcciones ni funciones de las versiones actuales.
Los perfiles que requieran funciones recientes de Xray no son compatibles.
Se recomienda actualizar macOS cuando sea posible.

La build verifica el minimo declarado de TODOS los Mach-O (10.15 o menor),
las dependencias y el arranque en el Mac de CI. No equivale a probar una
sesion de streaming en Catalina real: esa prueba queda pendiente.

Firma ad-hoc, sin notarizacion Apple. Si Gatekeeper impide abrirla, usa
clic derecho > Abrir o Preferencias del Sistema > Seguridad y privacidad
solo si confias en esta build. No desactives Gatekeeper globalmente.

Portable para ejecutar: los perfiles, ajustes y registros pueden quedar
en tu cuenta del Mac. No se incluyen perfiles ni credenciales personales.
Fuente GPLv3: https://github.com/vaaqdev/moonlight-xray-importer

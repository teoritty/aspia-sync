# Сборка

Ветка собирается только под Windows x64.

## Что нужно поставить

| Что | Чем |
|---|---|
| Visual Studio 2022 Build Tools, рабочая нагрузка C++ | `winget install Microsoft.VisualStudio.2022.BuildTools` |
| Компонент **ATL** | `setup.exe modify --installPath "<путь>" --add Microsoft.VisualStudio.Component.VC.ATL --quiet` |
| Компонент **MFC** | `setup.exe modify --installPath "<путь>" --add Microsoft.VisualStudio.Component.VC.ATLMFC --quiet` |
| CMake 3.21+ | `winget install Kitware.CMake` |
| Ninja | `winget install Ninja-build.Ninja` |

`setup.exe` лежит в `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer`, команды требуют прав администратора.

ATL и MFC нужны не сами по себе: пакет `wtl` из `vcpkg.json` тянет порты `atl` и `atlmfc`, а им требуются `atlbase.h` и `afxres.h` из набора инструментов. В базовую нагрузку C++ эти компоненты не входят, и без них сборка останавливается на установке зависимостей. `env.cmd` проверяет их наличие и говорит об этом сразу, а не через полчаса.

## Как собрать

```
git submodule update --init

tools\build\configure.cmd
tools\build\build.cmd
```

Либо одной командой, с тестами:

```
tools\build\verify.cmd
```

**Первая конфигурация идёт часами**: vcpkg собирает Qt5 из исходников под статический триплет. Последующие берут готовое из двоичного кэша и проходят за минуты. Под `builds/` уйдёт около 20 ГБ.

## Почему не штатные пресеты

В `CMakePresets.json` есть пресеты `ninja-multi-vcpkg-local-*`, но локально они не работают: `VCPKG_ROOT` в них указывает на каталог `vcpkg`, которого в репозитории нет (подмодуль называется `vcpkg4aspia`), а в Windows-пресете зашит путь к Ninja с машины другого разработчика. Скрипты используют пресет `ninja-multi-vcpkg-ci`, который берёт всё из переменных окружения.

## Если сборка падает странно

**Сотни ошибок внутри `vcruntime.h`, упоминания `C:\msys64`.** CMake нашёл зависимость (обычно zstd) в установке MinGW из `PATH` и притащил вместе с ней заголовки MinGW, несовместимые с заголовками MSVC. `env.cmd` запрещает CMake смотреть в такие префиксы через `CMAKE_IGNORE_PREFIX_PATH`; если у вас MinGW лежит в другом месте, добавьте его туда же.

**`Unable to locate 'atlbase.h'` или `'afxres.h'`.** Не хватает компонента ATL или MFC, см. таблицу выше.

**`Permission denied (publickey)` при инициализации подмодуля.** Адрес в `.gitmodules` переведён на https, так что ключ не нужен; ошибка означает, что у вас остался старый адрес в `.git/config`. Уберите его: `git config --unset submodule.vcpkg4aspia.url`.

## Правка сборочных скриптов

Файлы `.cmd` должны иметь переводы строк CRLF, иначе `cmd` разбирает их через строку и выдаёт бессмысленные сообщения вроде `|| was unexpected at this time`. В `.gitattributes` это задано правилом `*.cmd text eol=crlf`; при правке сторонним редактором следите, чтобы он не переписал их в LF.

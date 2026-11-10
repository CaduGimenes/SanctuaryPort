# SanctuaryPort — Diablo III (PS4) no PC

Port nativo da versão de PS4 de **Diablo III**: a versão brasileira (CUSA00243) com a
atualização 1.47, que é a **Eternal Collection** (com o Necromante), e a americana *Reaper of
Souls – Ultimate Evil Edition* 1.00 (CUSA00242). O código original do jogo roda direto no seu processador, um runtime substitui as
bibliotecas do sistema do PS4, e os gráficos do PS4 são traduzidos para Vulkan. Não há emulação
de CPU.

> **Nenhum arquivo do jogo vem junto.** Você precisa da sua própria cópia, tirada do seu PS4.
> Este projeto não tem ligação com a Blizzard nem com a Sony.

## Compilar (Linux)

Precisa de compilador C/C++ (clang ou gcc), CMake, Ninja, SDL3, Vulkan, Python 3.10+ com Pillow e
Rust 1.87+ (para o launcher). No Ubuntu 24.04, `bash packaging/linux/ci-deps.sh` instala tudo.

```bash
git clone --recursive https://github.com/CaduGimenes/SanctuaryPort.git
cd SanctuaryPort
bash build.sh
cargo build --release --manifest-path launcher-rust/Cargo.toml
```

## Instalar o jogo

1. Tenha os arquivos PKG do seu jogo (o jogo base, a atualização e, se tiver, as DLCs).
2. Abra o launcher (`launcher-rust/target/release/sanctuaryport-launcher`), vá em **Jogo** e use
   **Instalar a partir de PKG** escolhendo todos eles. O jogo vai para `~/Games/PS4/CUSA00243`,
   a atualização para `~/Games/PS4/CUSA00243-UPDATE` e as DLCs para `addcont/`. O launcher já
   configura as pastas.

Se você já tem o jogo extraído, é só apontar o launcher para a pasta que tem o `eboot.bin`.

## Jogar

Clique em **JOGAR** no launcher, ou pelo terminal:

```bash
GOW3_GAME_DIR=~/Games/PS4/CUSA00243 GOW3_PATCH_DIR=~/Games/PS4/CUSA00243-UPDATE bash run.sh
```

Os saves ficam em `user/savedata/`, com uma cópia de segurança em `user/savedata_backups/` a cada
início.

## Co-op local (até 4 jogadores)

Ligue mais controles (Xbox, DualSense, DualShock 4, qualquer um que o SDL reconheça). O primeiro é
o jogador 1, junto com o teclado e o mouse. Os seguintes viram os jogadores 2, 3 e 4, na ordem em
que você conecta. Com o jogo rodando, aperte o botão de baixo (A no Xbox, ✕ no PlayStation) no
controle novo para entrar: o Diablo abre a criação ou a escolha de herói daquele jogador.

## Botões do Xbox no jogo

No launcher: **Controles → Controle → Ícones dos botões no jogo → Xbox** (ou **Igual ao meu
controle**). Os botões do PlayStation nas dicas e no HUD viram os do Xbox (A, B, X, Y, LB, RB, LT,
RT, Menu, Visão). Os arquivos do jogo nunca são alterados.

## Controles

O controle (USB ou Bluetooth) funciona direto. No teclado (dá para mudar no launcher):

| Tecla | Botão do PS4 |
|---|---|
| W A S D | analógico esquerdo (andar) |
| setas | analógico direito (esquiva) |
| Espaço | ✕ (ataque / aceitar) |
| Shift esquerdo | Círculo |
| E | Quadrado |
| Q | Triângulo |
| 1 / 3 | L1 (poção) / R1 |
| R / F | L2 / R2 |
| Enter | Options |
| Tab | touchpad (inventário/menu) |
| Insert | menu do port (gráficos etc.) |
| F12 | captura de tela (`user/screenshots/`) |

Nome do herói: ao criar, digite no teclado e aperte Enter (ou ✕ / A no controle).

## Situação

- Funciona: tela de título, criação de herói, cinemáticas, Ato I, mundo a 60 FPS, salvar e
  continuar, DLCs, áudio, co-op local, sessões de mais de 20 minutos sem travar.
- Idiomas: os que o seu jogo traz. A versão brasileira tem português e espanhol; a americana tem
  inglês, francês e espanhol. Se o idioma escolhido não existir no jogo, o port usa o português
  (ou o primeiro disponível).
- Somente offline (sem PSN e sem co-op online).
- Testado no Linux. O código compila para Windows, mas esse caminho ainda não foi testado.

Detalhes técnicos: [docs/DIABLO3.md](docs/DIABLO3.md).

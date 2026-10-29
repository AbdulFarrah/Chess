# Chess

A terminal-based chess game written in C++17.

Repository: `AbdulFarrah/Chess`

The project supports human-vs-human, human-vs-AI, and AI-vs-AI games.
The AI can use the Stockfish chess engine through the UCI protocol,
while the program also contains its own move-selection heuristics for
its difficulty levels.

> **Platform:** Linux / Unix-like systems.
>
> The current implementation is not intended to be cross-platform. It
> directly uses POSIX/Linux APIs such as `fork()`, `pipe()`, `dup2()`,
> `execlp()`, `read()`, `write()`, and `waitpid()`.

## Features

-   Player vs Player
-   Player vs AI
-   AI vs AI
-   Play as White or Black
-   Five AI difficulty levels
-   Stockfish integration through the UCI protocol
-   Legal move generation
-   Check and checkmate detection
-   Stalemate detection
-   Insufficient-material detection
-   Automatic 75-move rule detection
-   Automatic fivefold-repetition detection
-   Castling
-   En passant
-   Pawn promotion
-   Captured-piece display
-   Move history
-   Last-move highlighting
-   Check highlighting
-   Legal-destination highlighting when selecting a piece
-   Debug logging to `chess_debug.log`

## Draw rules

The application automatically ends a game after 75 moves by each side
without a pawn move or capture (150 half-moves), or when the same position
has occurred five times. The bundled library exposes the half-move clock
and repetition count needed for these thresholds, so the app applies them
without modifying the generated `chess.hpp`.

The game does not offer draw claims at the 50-move or threefold-repetition
thresholds. The library's general `Board::isGameOver()` convenience method
uses those earlier thresholds, so the application uses its own game-end
check instead. Checkmate takes precedence over the automatic 75-move draw.
Insufficient-material detection uses the bundled library's basic checks.

## Requirements

### Operating system

A Linux/Unix-like environment with POSIX process and file-descriptor
APIs is required.

The code directly includes and uses:

-   `<unistd.h>`
-   `<sys/types.h>`
-   `<sys/wait.h>`
-   `fork()`
-   `pipe()`
-   `dup2()`
-   `execlp()`
-   `read()`
-   `write()`
-   `waitpid()`

These are used to start Stockfish as a child process and communicate
with it through the UCI protocol.

### C++ compiler

A C++17-compatible compiler is required.

The supplied build script uses:

``` bash
g++ -std=c++17 -Wall -Wextra -O2 main.cpp -o chess
```

GCC's C++17 support can be checked in the official GCC documentation:

https://gcc.gnu.org/projects/cxx-status.html

On Debian/Ubuntu, GCC can be installed with:

``` bash
sudo apt update
sudo apt install g++
```

On Fedora/RHEL-based systems:

``` bash
sudo dnf install gcc-c++
```

If your distribution uses another package manager, install the
equivalent GCC/G++ development package.

### Stockfish

The AI portion of this project expects Stockfish at this exact path:

``` text
/usr/games/stockfish
```

This path is currently hard-coded in `main.cpp`.

The official Stockfish download page provides Linux binaries:

https://stockfishchess.org/download/

The project does not bundle the Stockfish executable. You must install
it separately and make sure the executable is available at
`/usr/games/stockfish`.

After installation, verify it with:

``` bash
/usr/games/stockfish
```

Stockfish should start and accept UCI commands. Type:

``` text
uci
```

and look for:

``` text
uciok
```

Then exit with:

``` text
quit
```

If your Stockfish executable is installed somewhere else, either
place/link it at `/usr/games/stockfish` or modify `kEnginePath` in
`main.cpp`.

## Getting Started

Clone the repository:

``` bash
git clone https://github.com/AbdulFarrah/Chess.git
cd Chess
```

Install the required compiler and Stockfish, then build:

``` bash
chmod +x make.sh
./make.sh
```

The build script removes existing `.log` files and compiles the project
into:

``` text
chess
```

Run the game with:

``` bash
./chess
```

You can also build manually:

``` bash
g++ -std=c++17 -Wall -Wextra -O2 main.cpp -o chess
```

## Game Modes

When the program starts, choose one of:

``` text
1. Player vs Player
2. Player vs AI
3. AI vs AI
```

For Player vs AI, you can select the AI difficulty and choose whether to
play White or Black.

For AI vs AI, the program uses its highest configured difficulty.

## Difficulty Levels

The program exposes five difficulty levels. Level 4 is labeled **Human**
to describe the human-like style of its heuristic AI; it is still the
computer's turn. Level 5 uses Stockfish.

The exact behavior of each level is implemented in `getComputerMove()`
and the supporting move-evaluation functions in `main.cpp`.

The first four levels use simple heuristics, not a search engine. They
include techniques such as:

-   Piece-value evaluation
-   MVV-LVA capture ordering
-   Threat detection
-   Defensive move selection
-   Check detection
-   Safe-move evaluation
-   Level 4 reply checks that penalize moves leaving valuable pieces open
-   Random move selection when the heuristics have no preferred move

The heuristic names describe play styles rather than calibrated chess
strength. Level 5 uses Stockfish instead of those heuristics.

Stockfish itself is invoked through the UCI protocol with:

``` text
go movetime 1000
```

so the engine is given approximately one second of search time for a
move.

## How to Play

The game is played from the terminal.

When prompted to choose a piece, enter a square in this format:

``` text
e2
```

The program then displays the legal destinations for that piece.

You can select a move either by:

-   Entering its move number, or
-   Entering the destination square directly.

For example:

``` text
Legal moves from e2:
1. e2 -> e3
2. e2 -> e4

Enter move number or '0' to reselect:
```

Entering:

``` text
2
```

selects `e2 -> e4`.

Entering:

``` text
e4
```

also selects that move.

For a promotion, use its numbered entry so you can choose the piece.
Move history keeps the coordinate notation and appends the promotion
piece, for example `a7 -> a8 = Q`.

Enter:

``` text
0
```

to go back and select a different piece.

Enter:

``` text
q
```

or:

``` text
Q
```

to resign.

## Board Representation

The interface adapts to the terminal width and height. ANSI terminals get
screen clearing and move highlighting; simpler terminals get plain text.
Unicode piece symbols are used when the locale advertises UTF-8 and there
is enough terminal width; otherwise the board uses ASCII pieces (`K` for
White and `k` for Black). The layout reads the terminal window size when
available, uses a compact board in narrow windows, and limits displayed
move history based on terminal height. On very short screens it omits the
file labels to keep all eight ranks visible. Unicode glyphs are deliberately
reversed on dark backgrounds for contrast: White uses filled symbols and
Black uses hollow symbols. ASCII pieces continue to use uppercase for White
and lowercase for Black.

The interface also displays:

-   Current side to move
-   Check status
-   Recent move highlighting
-   Legal move highlighting
-   Captured pieces
-   Recent move history

## Stockfish Integration

The program communicates with Stockfish using the Universal Chess
Interface (UCI) protocol.

The basic flow is:

``` text
Chess program
     |
     | fork()
     v
Stockfish process
     ^
     |
     | pipes
     |
     +---- UCI commands
```

For an AI move, the program:

1.  Starts Stockfish as a child process.
2.  Connects the child's stdin/stdout using POSIX pipes.
3.  Sends `uci`.
4.  Waits for `uciok`.
5.  Sends `isready`.
6.  Waits for `readyok`.
7.  Sends the current board position as a FEN string.
8.  Sends the configured `go movetime 1000` command.
9.  Waits for a `bestmove` response.
10. Converts the UCI move into a legal internal chess move.
11. Applies the move to the board.
12. Terminates the Stockfish process.

UCI reads use timeouts, and shutdown escalates from a normal quit to
termination signals with bounded waits. If Stockfish fails to start, respond,
or return a legal move, it forfeits and the opponent wins. The game displays
the result and returns to the game menu after Enter.

The Stockfish executable path is defined in `main.cpp`:

``` cpp
static const char *kEnginePath = "/usr/games/stockfish";
```

## Logging

The program writes diagnostic information to:

``` text
chess_debug.log
```

The log contains information about game flow, move selection, captures,
Stockfish communication, and other internal operations.

The supplied `make.sh` removes existing `.log` files before rebuilding.
If Stockfish cannot start, respond, or provide a legal move, it forfeits;
the opponent is declared the winner. Press Enter after the result to return
to the game menu.

## Project Structure

A typical repository layout is:

``` text
Chess/
├── main.cpp
├── chess.hpp
├── make.sh
└── README.md
```

### `main.cpp`

Contains the application itself, including:

-   Terminal UI
-   Input handling
-   Game loop
-   Player move selection
-   AI move selection
-   Stockfish/UCI communication
-   Move history
-   Captured-piece tracking
-   Debug logging

### `chess.hpp`

A single-header chess library used by the application for the chess
rules and board representation.

It provides functionality including:

-   Board representation
-   Pieces and squares
-   Bitboards
-   Legal move generation
-   Check detection
-   Castling
-   En passant
-   Promotion
-   FEN handling
-   UCI move conversion
-   SAN/LAN move conversion
-   PGN parsing support
-   Game-state detection

The bundled header identifies itself as **Disservin's `chess-library`**
and contains its own upstream license and copyright notice. See the
header itself for the complete license text and attribution.

This project does **not** claim ownership of the upstream chess-library
code.

### `make.sh`

Build helper script.

Current build command:

``` bash
rm -f ./*.log
g++ -std=c++17 -Wall -Wextra -O2 main.cpp -o chess
```

## Important Notes

### Stockfish is a separate dependency

Stockfish is not included in this repository. It is a separate
open-source chess engine distributed under GPLv3.

If you redistribute Stockfish together with this project, follow the
Stockfish GPLv3 licensing and source-code requirements.

Official information:

https://stockfishchess.org/about/

Official downloads:

https://stockfishchess.org/download/

### No project-wide license

The repository is public and available at no charge to view and download.
No license is declared for the project-authored files, so public
availability alone does not grant reuse rights.

The `chess.hpp` file contains the MIT license for Disservin's upstream
chess-library code. Those terms apply to that upstream code, not to the
project-authored files.

## Troubleshooting

### `Stockfish` cannot be started

If the AI cannot obtain a move, first check:

``` bash
ls -l /usr/games/stockfish
```

Then:

``` bash
/usr/games/stockfish
```

If Stockfish is installed elsewhere, update `kEnginePath` in `main.cpp`.

### Permission denied when running Stockfish

Make sure the executable has execute permission:

``` bash
chmod +x /usr/games/stockfish
```

### `g++: command not found`

Install GCC/G++ using your Linux distribution's package manager.

### `./make.sh: Permission denied`

Run:

``` bash
chmod +x make.sh
```

then:

``` bash
./make.sh
```

### Chess symbols look incorrect

Set a UTF-8 locale to use Unicode chess symbols. Without one, the program
uses ASCII piece letters. ANSI highlighting is only used when stdout is a
terminal that does not identify itself as `dumb`.

## Credits

-   **Project:** AbdulFarrah/Chess
-   **Chess rules / board library:** Disservin `chess-library` bundled
    as `chess.hpp`
-   **Chess engine:** Stockfish

Stockfish is developed by the Stockfish community and is distributed
under the GNU GPLv3.

## Links

-   [Stockfish](https://stockfishchess.org/)
-   [Stockfish Downloads](https://stockfishchess.org/download/)
-   [GCC](https://gcc.gnu.org/)
-   [GCC C++17 Support](https://gcc.gnu.org/projects/cxx-status.html)

------------------------------------------------------------------------

Built as a terminal-based C++ chess project.

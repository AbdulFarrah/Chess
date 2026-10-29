#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include "chess.hpp"
#include <fstream>
#include <unistd.h>
#include <random>
#include <limits>
#include <iomanip>
#include <array>
#include <optional>
#include <chrono>
#include <thread>
#include <sstream>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <regex>
#include <charconv>
#include <poll.h>
#include <csignal>
#include <sys/ioctl.h>

static const char *kEnginePath = "/usr/games/stockfish";
static const char *kGoCommand = "go movetime 1000";

void debugLog(const std::string &msg, bool firstRun = false)
{
	static bool fileCleared = false;
	const std::string logFile = "chess_debug.log";
	static std::ofstream log;
	if (!fileCleared && firstRun)
	{
		if (log.is_open()) log.close();
		std::remove(logFile.c_str());
		fileCleared = true;
	}
	if (!log.is_open()) log.open(logFile, std::ios::app);
	if (log.is_open())
	{
		log << msg << "\n";
	}
}

static std::optional<chess::Move> uci_to_move_on_board(const std::string &uci, const chess::Board &board)
{
	// debugLog("uci_to_move_on_board called with UCI string: " + uci);
	if (uci.size() < 4)
	{
		debugLog("UCI string too short, returning nullopt");
		return std::nullopt;
	}
	chess::Square from(uci.substr(0, 2));
	chess::Square to(uci.substr(2, 2));
	char promo = (uci.size() >= 5) ? std::tolower(static_cast<unsigned char>(uci[4])) : '\0';
	debugLog("Parsed UCI move: from=" + static_cast<std::string>(from) +
			 " to=" + static_cast<std::string>(to) +
			 (promo ? (" promo=" + std::string(1, promo)) : " no-promo"));
	chess::Movelist legal;
	chess::movegen::legalmoves<chess::movegen::MoveGenType::ALL>(legal, board);
	// debugLog("Generated " + std::to_string(legal.size()) + " legal moves to check against UCI move");
	auto match = [&](const chess::Move &m) -> bool
	{
		if (m.from() != from || m.to() != to)
			return false;
		if (promo == '\0')
		{
			bool ok = m.typeOf() != chess::Move::PROMOTION;
			if (ok)
			{
				// debugLog("Matched non-promotion move: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
			}
			return ok;
		}
		else
		{
			if (m.typeOf() != chess::Move::PROMOTION)
				return false;
			chess::PieceType want = chess::PieceType::QUEEN;
			if (promo == 'q')
				want = chess::PieceType::QUEEN;
			else if (promo == 'r')
				want = chess::PieceType::ROOK;
			else if (promo == 'b')
				want = chess::PieceType::BISHOP;
			else if (promo == 'n')
				want = chess::PieceType::KNIGHT;
			else
				return false;
			bool ok = m.promotionType() == want;
			if (ok)
			{
				// debugLog("Matched promotion move: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()) + " promoted to " + std::to_string(static_cast<int>(want)));
			}
			return ok;
		}
	};
	for (int i = 0; i < legal.size(); ++i)
	{
		if (match(legal[i]))
		{
			// debugLog("UCI move successfully matched to legal move on board");
			return legal[i];
		}
	}
	debugLog("No matching legal move found for UCI string: " + uci);
	return std::nullopt;
}

static bool uci_write(int fd, const std::string &s)
{
	std::string line = s;
	if (line.empty() || line.back() != '\n')
		line.push_back('\n');
	size_t offset = 0;
	while (offset < line.size())
	{
		ssize_t wrote = ::write(fd, line.data() + offset, line.size() - offset);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return false;
		offset += static_cast<size_t>(wrote);
	}
	return true;
}

static std::optional<std::string> uci_read_until(int fd, const std::string &starts_with, int timeout_ms = 5000)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	std::string acc;
	char buf[512];
	// debugLog("uci_read_until called: fd=" + std::to_string(fd) + " starts_with=\"" + starts_with + "\"" + " timeout_ms=" + std::to_string(timeout_ms));
	while (true)
	{
		auto now = std::chrono::steady_clock::now();
		int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
		if (remaining <= 0)
			return std::nullopt;
		pollfd descriptor{fd, POLLIN, 0};
		int ready;
		do
		{
			ready = ::poll(&descriptor, 1, remaining);
		} while (ready < 0 && errno == EINTR);
		if (ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)))
			return std::nullopt;
		ssize_t n;
		do
		{
			n = ::read(fd, buf, sizeof(buf) - 1);
		} while (n < 0 && errno == EINTR);
		if (n <= 0)
		{
			debugLog("uci_read_until: read() returned " + std::to_string(n) + " -> EOF or error");
			return std::nullopt;
		}
		buf[n] = '\0';
		acc += buf;
		// debugLog("uci_read_until: read " + std::to_string(n) + " bytes, buffer=\"" + std::string(buf) + "acc_size=" + std::to_string(acc.size()));
		size_t pos;
		while ((pos = acc.find('\n')) != std::string::npos)
		{
			std::string line = acc.substr(0, pos);
			acc.erase(0, pos + 1);
			// debugLog("UCI<- " + line);
			if (starts_with.empty())
			{
				debugLog("uci_read_until: returning line (no starts_with required)");
				return line;
			}
			if (line.rfind(starts_with, 0) == 0)
			{
				// debugLog("uci_read_until: matched starts_with -> returning \"" + line + "\"");
				return line;
			}
		}
	}
}

static void stopEngine(pid_t pid, int child_in, int child_out)
{
	if (child_in >= 0)
	{
		uci_write(child_in, "quit");
		close(child_in);
	}
	if (child_out >= 0)
		close(child_out);
	if (pid <= 0)
		return;
	auto waitBriefly = [pid](int milliseconds)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		while (std::chrono::steady_clock::now() < deadline)
		{
			pid_t result = waitpid(pid, nullptr, WNOHANG);
			if (result == pid || (result < 0 && errno == ECHILD))
				return true;
			if (result < 0 && errno != EINTR)
				return true;
			usleep(10000);
		}
		return false;
	};
	if (waitBriefly(500))
		return;
	kill(pid, SIGTERM);
	if (waitBriefly(500))
		return;
	kill(pid, SIGKILL);
	if (!waitBriefly(1000))
		debugLog("UCI: engine did not reap within timeout after SIGKILL");
}

chess::Move get_best_move_from_stockfish(const chess::Board &board)
{
	// debugLog("UCI: starting engine", true);
	int to_child[2] = {-1, -1};
	int from_child[2] = {-1, -1};
	if (pipe(to_child) < 0)
	{
		debugLog("UCI: input pipe() failed");
		return chess::Move(chess::Move::NO_MOVE);
	}
	if (pipe(from_child) < 0)
	{
		debugLog("UCI: output pipe() failed");
		close(to_child[0]);
		close(to_child[1]);
		return chess::Move(chess::Move::NO_MOVE);
	}
	// debugLog("UCI: pipes created successfully");
	pid_t pid = fork();
	if (pid == -1)
	{
		debugLog("UCI: fork() failed");
		close(to_child[0]);
		close(to_child[1]);
		close(from_child[0]);
		close(from_child[1]);
		return chess::Move(chess::Move::NO_MOVE);
	}
	if (pid == 0)
	{
		// debugLog("UCI: in child process, setting up redirection before execlp()");
		if (dup2(to_child[0], STDIN_FILENO) < 0 ||
			dup2(from_child[1], STDOUT_FILENO) < 0 ||
			dup2(from_child[1], STDERR_FILENO) < 0)
			_exit(126);
		close(to_child[1]);
		close(from_child[0]);
		close(to_child[0]);
		close(from_child[1]);
		// debugLog("UCI: execlp -> " + std::string(kEnginePath));
		execlp(kEnginePath, kEnginePath, (char *)nullptr);
		_exit(127);
	}
	// debugLog("UCI: in parent process, child pid = " + std::to_string(pid));
	close(to_child[0]);
	close(from_child[1]);
	int child_in = to_child[1];
	int child_out = from_child[0];
	// debugLog("UCI: parent connected to child (child_in=" + std::to_string(child_in) + ", child_out=" + std::to_string(child_out) + ")");
	if (!uci_write(child_in, "uci"))
	{
		debugLog("UCI: failed to send startup command");
		stopEngine(pid, child_in, child_out);
		return chess::Move(chess::Move::NO_MOVE);
	}
	// debugLog("UCI: sent 'uci' command");
	if (!uci_read_until(child_out, "uciok"))
	{
		debugLog("UCI: no uciok (engine did not respond to 'uci')");
		stopEngine(pid, child_in, child_out);
		return chess::Move(chess::Move::NO_MOVE);
	}
	// debugLog("UCI: got uciok");
	if (!uci_write(child_in, "isready"))
	{
		debugLog("UCI: failed to send readiness command");
		stopEngine(pid, child_in, child_out);
		return chess::Move(chess::Move::NO_MOVE);
	}
	// debugLog("UCI: sent 'isready'");
	if (!uci_read_until(child_out, "readyok"))
	{
		debugLog("UCI: no readyok (engine not ready)");
		stopEngine(pid, child_in, child_out);
		return chess::Move(chess::Move::NO_MOVE);
	}
	// debugLog("UCI: got readyok");
	std::string fen = board.getFen();
	debugLog("UCI: sending position fen: " + fen);
	if (!uci_write(child_in, "position fen " + fen) || !uci_write(child_in, kGoCommand))
	{
		debugLog("UCI: failed to send position or search command");
		stopEngine(pid, child_in, child_out);
		return chess::Move(chess::Move::NO_MOVE);
	}
	// debugLog("UCI: sending go command: " + std::string(kGoCommand));
	std::optional<std::string> bestLine;
	// debugLog("UCI: waiting for bestmove...");
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		std::string acc;
		char buf[512];
		// debugLog("UCI: entering bestmove read loop...");
		while (true)
		{
			auto now = std::chrono::steady_clock::now();
			int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
			if (remaining <= 0)
				break;
			pollfd descriptor{child_out, POLLIN, 0};
			int ready;
			do
			{
				ready = ::poll(&descriptor, 1, remaining);
			} while (ready < 0 && errno == EINTR);
			if (ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)))
				break;
			ssize_t n;
			do
			{
				n = ::read(child_out, buf, sizeof(buf) - 1);
			} while (n < 0 && errno == EINTR);
			if (n <= 0)
			{
				debugLog("UCI: read() returned <= 0 while waiting for bestmove");
				break;
			}
			buf[n] = '\0';
			acc += buf;
			size_t pos;
			while ((pos = acc.find('\n')) != std::string::npos)
			{
				std::string line = acc.substr(0, pos);
				acc.erase(0, pos + 1);
				// debugLog("UCI<- " + line);
				if (line.rfind("bestmove ", 0) == 0)
				{
					bestLine = line;
					debugLog("UCI: bestmove line received: " + line);
					break;
				}
			}
			if (bestLine)
			{
				// debugLog("UCI: breaking read loop after bestmove");
				break;
			}
		}
		// debugLog("UCI: exiting bestmove read loop");
	}
	stopEngine(pid, child_in, child_out);
	// debugLog("UCI: engine process terminated and pipes closed");
	if (!bestLine)
	{
		debugLog("UCI: no bestmove line received (engine gave no valid response)");
		return chess::Move(chess::Move::NO_MOVE);
	}
	// debugLog("UCI: raw bestmove line: " + *bestLine);
	std::istringstream iss(*bestLine);
	std::string tok, uciMove;
	iss >> tok;
	iss >> uciMove;
	if (uciMove == "(none)" || uciMove.size() < 4)
	{
		debugLog("UCI: invalid bestmove token received: " + uciMove);
		return chess::Move(chess::Move::NO_MOVE);
	}
	// debugLog("UCI: parsed bestmove token: " + uciMove);
	auto mv = uci_to_move_on_board(uciMove, board);
	if (!mv)
	{
		debugLog("UCI: bestmove not legal on current board: " + uciMove);
		return chess::Move(chess::Move::NO_MOVE);
	}
	debugLog("UCI: bestmove accepted, move resolved to: " +
			 static_cast<std::string>((*mv).from()) + " -> " + static_cast<std::string>((*mv).to()));
	return *mv;
}

std::optional<std::string> getLineInput()
{
	debugLog("Input: waiting for line from std::cin");
	std::string line;
	if (!std::getline(std::cin, line))
		return std::nullopt;
	debugLog("Input: raw line read = '" + line + "'");
	if (line.size() > 2) line.resize(2);
	return line;
}

bool isBait(const chess::Board &board, const chess::Move &move, bool iswhite)
{
	chess::Board temp = board;
	temp.makeMove(move);
	auto colour = iswhite ? chess::Color::WHITE : chess::Color::BLACK;
	if (temp.isAttacked(move.to(), colour))
	{
		debugLog("Bait detected: Move " +
				 static_cast<std::string>(move.from()) + " -> " +
				 static_cast<std::string>(move.to()) +
				 " is attacked by opponent after move.");
		return true;
	}
	debugLog("Safe move: " +
			 static_cast<std::string>(move.from()) + " -> " +
			 static_cast<std::string>(move.to()));
	return false;
}

int pieceValue(chess::PieceType type)
{
	int val = 0;
	if (type == chess::PieceType::QUEEN)
		val = 9;
	else if (type == chess::PieceType::ROOK)
		val = 5;
	else if (type == chess::PieceType::BISHOP)
		val = 3;
	else if (type == chess::PieceType::KNIGHT)
		val = 3;
	else if (type == chess::PieceType::PAWN)
		val = 1;
	else
		val = 0;
	debugLog("pieceValue queried: " + std::to_string(static_cast<int>(type)) +
			 " -> value=" + std::to_string(val));
	return val;
}

chess::Piece capturedPiece(const chess::Board &board, const chess::Move &move)
{
	if (move.typeOf() == chess::Move::CASTLING) return chess::Piece::NONE;
	if (move.typeOf() == chess::Move::ENPASSANT)
	{
		chess::Square square = board.sideToMove() == chess::Color::WHITE ? move.to() - 8 : move.to() + 8;
		return board.at(square);
	}
	return board.at(move.to());
}

std::string moveDescription(const chess::Move &move)
{
	std::string description = static_cast<std::string>(move.from()) + " -> " + static_cast<std::string>(move.to());
	if (move.typeOf() == chess::Move::PROMOTION)
	{
		description += " = ";
		if (move.promotionType() == chess::PieceType::QUEEN) description += "Q";
		else if (move.promotionType() == chess::PieceType::ROOK) description += "R";
		else if (move.promotionType() == chess::PieceType::BISHOP) description += "B";
		else if (move.promotionType() == chess::PieceType::KNIGHT) description += "N";
	}
	return description;
}

bool moveCausesCheck(const chess::Board &board, const chess::Move &move)
{
	chess::Board temp = board;
	temp.makeMove(move);
	bool causesCheck = temp.inCheck();
	debugLog("moveCausesCheck: " + static_cast<std::string>(move.from()) +
			 " -> " + static_cast<std::string>(move.to()) +
			 " causesCheck=" + (causesCheck ? "true" : "false"));
	return causesCheck;
}

bool isSafeMove(const chess::Board &board, const chess::Move &move)
{
	chess::Board temp = board;
	temp.makeMove(move);
	chess::Movelist oppMoves;
	chess::movegen::legalmoves(oppMoves, temp);
	for (auto m : oppMoves)
	{
		if (m.to() == move.to())
		{
			debugLog("isSafeMove: " + static_cast<std::string>(move.from()) +
					 " -> " + static_cast<std::string>(move.to()) + " is NOT safe (can be captured)");
			return false;
		}
	}
	debugLog("isSafeMove: " + static_cast<std::string>(move.from()) +
			 " -> " + static_cast<std::string>(move.to()) + " is safe");
	return true;
}

int mvvLvaValue(chess::PieceType victim, chess::PieceType attacker)
{
	int victimVal = pieceValue(victim);
	int attackerVal = pieceValue(attacker);
	int score = victimVal * 10 - attackerVal;
	debugLog("MVV-LVA: victim=" + std::to_string(victimVal) +
			 ", attacker=" + std::to_string(attackerVal) +
			 ", score=" + std::to_string(score));

	return score;
}

chess::Move pickMVVLVA(const chess::Movelist &moves, const chess::Board &board)
{
	int bestScore = -99999;
	chess::Move bestMove = chess::Move::NO_MOVE;
	for (auto m : moves)
	{
		chess::Piece attacker = board.at(m.from());
		chess::Piece victim = capturedPiece(board, m);
		if (victim != chess::Piece::NONE && victim.color() != attacker.color())
		{
			int score = mvvLvaValue(victim.type(), attacker.type());
			debugLog("MVV-LVA candidate: " + static_cast<std::string>(m.from()) + " -> " +
					 static_cast<std::string>(m.to()) + " | attacker=" +
					 static_cast<std::string>(attacker.type()) + " victim=" +
					 static_cast<std::string>(victim.type()) +
					 " score=" + std::to_string(score));
			if (score > bestScore)
			{
				bestScore = score;
				bestMove = m;
			}
		}
	}
	if (bestMove != chess::Move::NO_MOVE)
	{
		debugLog("MVV-LVA picked: " + static_cast<std::string>(bestMove.from()) + " -> " +
				 static_cast<std::string>(bestMove.to()) + " with score=" + std::to_string(bestScore));
	}
	else
	{
		debugLog("MVV-LVA found no capture.");
	}
	return bestMove;
}

struct Threat
{
	chess::Move move;
	chess::Square targetSq;
	chess::PieceType t;
	chess::Square threateningPieceSq;
	int value;
};

struct ThreatAnalysis
{
	std::vector<Threat> threats;
	Threat worst;
	chess::Move bestDefense;
	std::vector<chess::Move> defenseMoves;
	bool inCheck = false;
	int totalthreatvalue = 0;
};

ThreatAnalysis detectThreatAndBlock(const chess::Board &board, const chess::Movelist &moves, bool iswhite)
{
	ThreatAnalysis defenseforthreat;
	auto colour = iswhite ? chess::Color::BLACK : chess::Color::WHITE;
	auto colour2 = iswhite ? chess::Color::WHITE : chess::Color::BLACK;
	if (board.sideToMove() != colour)
	{
		defenseforthreat.bestDefense = chess::Move::NO_MOVE;
		return defenseforthreat;
	}
	if (board.sideToMove() == colour && !board.inCheck())
	{
		chess::Board tmp = board;
		tmp.makeNullMove();
		chess::Movelist oppMoves;
		chess::movegen::legalmoves<chess::movegen::MoveGenType::ALL>(oppMoves, tmp);
		for (const auto &m : oppMoves)
		{
			chess::Square capturedSq;
			chess::Square dummy = chess::Square::NO_SQ;
			chess::Piece targetPiece = chess::Piece::NONE;
			if (m.typeOf() == chess::Move::ENPASSANT)
			{
				int toIdx = m.to().index();
				capturedSq = chess::Square(toIdx - 8);
				targetPiece = board.at(capturedSq);
			}
			else
			{
				capturedSq = m.to();
				targetPiece = board.at(capturedSq);
			}
			if (targetPiece != chess::Piece::NONE && targetPiece.color() == colour)
			{
				int val = pieceValue(targetPiece.type());
				if (moveCausesCheck(tmp, m))
					val += 50;
				debugLog("Threat detected: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()) + " capturing " + static_cast<std::string>(targetPiece.type()) + " (score=" + std::to_string(val) + ")");
				defenseforthreat.threats.push_back({m, capturedSq, targetPiece.type(), dummy, val});
			}
		}
		if (defenseforthreat.threats.empty())
		{
			debugLog("NO Threat");
			defenseforthreat.bestDefense = chess::Move::NO_MOVE;
			return defenseforthreat;
		}
		defenseforthreat.worst = *std::max_element(defenseforthreat.threats.begin(), defenseforthreat.threats.end(), [](const Threat &a, const Threat &b)
												   { return a.value < b.value; });
		debugLog("Worst threat targets square " + static_cast<std::string>(defenseforthreat.worst.targetSq) + " value=" + std::to_string(defenseforthreat.worst.value) + " from " + static_cast<std::string>(defenseforthreat.worst.move.from()));
	}
	chess::Movelist defMoves;
	chess::movegen::legalmoves<chess::movegen::MoveGenType::ALL>(defMoves, board);
	chess::Move bestMove;
	int bestPriority = 0;
	if (board.sideToMove() == colour && board.inCheck())
	{
		defMoves = moves;
		defenseforthreat.worst.targetSq = board.kingSq(colour);
		debugLog("King is in check at " + static_cast<std::string>(defenseforthreat.worst.targetSq));
		chess::Bitboard atks = chess::attacks::attackers(board, colour2, defenseforthreat.worst.targetSq);
		if (atks)
		{
			defenseforthreat.worst.threateningPieceSq = atks.pop();
			defenseforthreat.worst.move = chess::Move::make<chess::Move::NORMAL>(defenseforthreat.worst.threateningPieceSq, defenseforthreat.worst.targetSq);
			debugLog("King is attacked from: " + static_cast<std::string>(defenseforthreat.worst.threateningPieceSq));
		}
		defenseforthreat.threats.push_back(defenseforthreat.worst);
		defenseforthreat.inCheck = true;
	}
	for (const auto &dm : defMoves)
	{
		chess::Board after = board;
		after.makeMove(dm);
		if (dm.to() == defenseforthreat.worst.move.from())
		{
			if (!after.isAttacked(dm.to(), colour2))
			{
				debugLog("Defense succeeds by safely capturing threat: " + static_cast<std::string>(dm.from()) + " -> " + static_cast<std::string>(dm.to()));
				defenseforthreat.defenseMoves.push_back(dm);
				defenseforthreat.bestDefense = dm;
				for (auto &t : defenseforthreat.threats)
				{
					defenseforthreat.totalthreatvalue += t.value;
				}
				return defenseforthreat;
			}
			else
			{
				debugLog("Capture of threat is unsafe: " + static_cast<std::string>(dm.from()) + " -> " + static_cast<std::string>(dm.to()));
				continue;
			}
		}
		if (!after.isAttacked(defenseforthreat.worst.targetSq, colour2))
		{
			debugLog("Defense candidate: block threat " + static_cast<std::string>(dm.from()) + " -> " + static_cast<std::string>(dm.to()));
			bestMove = dm;
			bestPriority = 2;
			defenseforthreat.defenseMoves.push_back(dm);
			continue;
		}
		chess::Piece post = after.at(defenseforthreat.worst.targetSq);
		if (post == chess::Piece::NONE || post.color() != colour)
		{
			debugLog("Defense candidate: move away " + static_cast<std::string>(dm.from()) + " -> " + static_cast<std::string>(dm.to()));
			if (bestPriority < 1)
			{
				bestMove = dm;
				bestPriority = 1;
				defenseforthreat.defenseMoves.push_back(dm);
			}
		}
	}
	if (bestPriority > 0)
	{
		if (bestPriority == 2)
		{
			debugLog("Defense succeeds by blocking threat: " + static_cast<std::string>(bestMove.from()) + " -> " + static_cast<std::string>(bestMove.to()));
		}
		else
		{
			debugLog("Defense succeeds by moving away: " + static_cast<std::string>(bestMove.from()) + " -> " + static_cast<std::string>(bestMove.to()));
		}
		defenseforthreat.bestDefense = bestMove;
		for (auto &t : defenseforthreat.threats)
		{
			defenseforthreat.totalthreatvalue += t.value;
		}
		return defenseforthreat;
	}
	debugLog("Could not resolve threat, checking for a capture");
	defenseforthreat.bestDefense = pickMVVLVA(defMoves, board);
	for (auto &t : defenseforthreat.threats)
	{
		defenseforthreat.totalthreatvalue += t.value;
	}
	return defenseforthreat;
}

struct ScoredMove
{
	chess::Move move{};
	int score = 0;
	bool givesCheck = false;
	bool isPromotion = false;
	int capturedValue = 0;
};

bool compareScoredMoves(const ScoredMove &a, const ScoredMove &b)
{
	if (a.score != b.score)
		return a.score > b.score;

	if (a.givesCheck != b.givesCheck)
		return a.givesCheck > b.givesCheck;

	if (a.capturedValue != b.capturedValue)
		return a.capturedValue > b.capturedValue;

	if (a.isPromotion != b.isPromotion)
		return a.isPromotion > b.isPromotion;

	return false;
}

int penalizeIfExposesKing(const chess::Board &board, const chess::Move &m)
{
	chess::Board after = board;
	after.makeMove(m);
	chess::Movelist oppMoves;
	chess::movegen::legalmoves<chess::movegen::MoveGenType::ALL>(oppMoves, after);
	int maxThreat = 0;
	for (const auto &om : oppMoves)
	{
		chess::Piece target = after.at(om.to());
		if (target != chess::Piece::NONE && target.color() == board.sideToMove())
		{
			int val = pieceValue(target.type());
			if (val > maxThreat)
				maxThreat = val;
		}
		else if (om.typeOf() == chess::Move::ENPASSANT)
		{
			// The captured pawn is beside the destination square in en passant.
			const chess::Square capturedSquare(om.to().file(), om.from().rank());
			chess::Piece epTarget = after.at(capturedSquare);
			if (epTarget != chess::Piece::NONE && epTarget.color() == board.sideToMove())
				maxThreat = std::max(maxThreat, pieceValue(epTarget.type()));
		}
	}
	return maxThreat * 2;
}

chess::Move getComputerMove(const chess::Board &board, int difficulty, bool iswhite)
{
	chess::Movelist moves;
	chess::movegen::legalmoves(moves, board);
	if (moves.empty())
	{
		debugLog("Computer has no legal moves (game over).");
		return chess::Move::NO_MOVE;
	}
	debugLog("Computer evaluating " + std::to_string(moves.size()) + " legal moves at difficulty " + std::to_string(difficulty));
	static std::mt19937 rng(std::random_device{}());
	auto randomMove = [&]()
	{
		std::uniform_int_distribution<int> dist(0, moves.size() - 1);
		chess::Move m = moves[dist(rng)];
		debugLog("Chose random move: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
		return m;
	};
	if (difficulty == 1)
	{
		debugLog("Difficulty 1: checking for capture moves.");
		std::vector<chess::Move> captures;
		for (auto m : moves)
		{
			auto targetPiece = capturedPiece(board, m);
			if (targetPiece != chess::Piece::NONE && targetPiece.type() != chess::PieceType::KING)
			{
				captures.push_back(m);
			}
			else if (targetPiece.type() == chess::PieceType::KING)
			{
				debugLog("Skipping move that captures king: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
			}
		}
		if (!captures.empty())
		{
			chess::Move m = captures[rng() % captures.size()];
			debugLog("Selected capture move: " + moveDescription(m) +
					 " (target piece: " + std::to_string(pieceValue(capturedPiece(board, m).type())) + ")");
			return m;
		}
		debugLog("No capture moves found, selecting random move.");
		return randomMove();
	}
	if (difficulty == 2)
	{
		debugLog("Difficulty 2: checking for threat blocking.");
		ThreatAnalysis temp = detectThreatAndBlock(board, moves, iswhite);
		chess::Move defensive;
		defensive = temp.bestDefense;
		if (defensive != chess::Move::NO_MOVE)
		{
			return defensive;
		}
		debugLog("Difficulty 2: No threat,checking for capture moves.");
		std::vector<chess::Move> captures;
		for (auto m : moves)
		{
			auto targetPiece = capturedPiece(board, m);
			if (targetPiece != chess::Piece::NONE && targetPiece.type() != chess::PieceType::KING)
			{
				captures.push_back(m);
			}
			else if (targetPiece.type() == chess::PieceType::KING)
			{
				debugLog("Skipping move that captures king: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
			}
		}
		if (!captures.empty())
		{
			chess::Move m = captures[rng() % captures.size()];
			debugLog("Selected capture move: " + moveDescription(m) +
					 " (target piece: " + std::to_string(pieceValue(capturedPiece(board, m).type())) + ")");
			return m;
		}
		debugLog("No capture moves found, checking for safe moves.");
		std::vector<chess::Move> safeMoves;
		for (auto m : moves)
		{
			if (isSafeMove(board, m))
				safeMoves.push_back(m);
		}
		if (!safeMoves.empty())
		{
			chess::Move m = safeMoves[rng() % safeMoves.size()];
			debugLog("Selected safe move: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
			return m;
		}
		debugLog("No safe moves found, falling back to random move.");
		return randomMove();
	}
	if (difficulty == 3)
	{
		debugLog("Difficulty 3: checking for moves that cause check.");
		std::vector<chess::Move> safeChecks;
		for (auto m : moves)
		{
			if (moveCausesCheck(board, m) && isSafeMove(board, m))
			{
				safeChecks.push_back(m);
			}
		}
		if (!safeChecks.empty())
		{
			chess::Move m = safeChecks[rng() % safeChecks.size()];
			debugLog("Selected checking move: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
			return m;
		}
		debugLog("Difficulty 3: checking for threat blocking.");
		ThreatAnalysis temp = detectThreatAndBlock(board, moves, iswhite);
		chess::Move defensive;
		defensive = temp.bestDefense;
		if (defensive != chess::Move::NO_MOVE)
		{
			return defensive;
		}
		debugLog("No threat, checking for promotion.");
		std::vector<chess::Move> promotions;
		for (auto m : moves)
		{
			if (m.typeOf() == chess::Move::PROMOTION)
			{
				promotions.push_back(m);
			}
		}
		if (!promotions.empty())
		{
			chess::Move m = promotions[rng() % promotions.size()];
			debugLog("Selected Promotion move: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
			return m;
		}
		debugLog("No promotion moves, checking for captures.");
		std::vector<chess::Move> captures;
		for (auto m : moves)
		{
			auto targetPiece = capturedPiece(board, m);
			if (targetPiece != chess::Piece::NONE && targetPiece.type() != chess::PieceType::KING)
			{
				captures.push_back(m);
			}
			else if (targetPiece.type() == chess::PieceType::KING)
			{
				debugLog("Skipping move that captures king: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
			}
		}
		if (!captures.empty())
		{
			std::sort(captures.begin(), captures.end(), [&](const chess::Move &a, const chess::Move &b)
						  { return pieceValue(capturedPiece(board, a).type()) > pieceValue(capturedPiece(board, b).type()); });
			for (auto &m : captures)
			{
				if (!isBait(board, m, iswhite))
				{
					debugLog("Selected high-value capture: " + moveDescription(m) +
							 " (capturing value " + std::to_string(pieceValue(capturedPiece(board, m).type())) + ")");
					return m;
				}
			}
			debugLog("All high-value captures are bait, fallback to safe landing.");
		}
		debugLog("Checking for safe moves.");
		std::vector<chess::Move> safeMoves;
		for (auto m : moves)
		{
				if (isSafeMove(board, m))
				safeMoves.push_back(m);
		}
		if (!safeMoves.empty())
		{
			chess::Move m = safeMoves[rng() % safeMoves.size()];
			debugLog("Selected safe move: " + static_cast<std::string>(m.from()) + " -> " + static_cast<std::string>(m.to()));
			return m;
		}
		debugLog("No strategic options, falling back to random move.");
		return randomMove();
	}
	if (difficulty == 4)
	{
		ThreatAnalysis tinfo = detectThreatAndBlock(board, moves, iswhite);
		std::vector<ScoredMove> scoredMoves;
		for (auto m : moves)
		{
			chess::Board after = board;
			after.makeMove(m);
			int score = 0;
			bool safe = isSafeMove(board, m);
			bool check = moveCausesCheck(board, m);
			bool promotion = (m.typeOf() == chess::Move::PROMOTION);
			chess::Piece target = capturedPiece(board, m);
			int capturedVal = 0;
			int attackerVal = pieceValue(board.at(m.from()).type());
			if (target != chess::Piece::NONE)
			{
				capturedVal = pieceValue(target.type());
				int mvvLvaScore = capturedVal * 10 - attackerVal;
				score += mvvLvaScore;
				debugLog("MVV-LVA: " + static_cast<std::string>(m.from()) + " -> " +
						 static_cast<std::string>(m.to()) + " capture " +
						 static_cast<std::string>(target.type()) +
						 " (victim=" + std::to_string(capturedVal) +
						 ", attacker=" + std::to_string(attackerVal) +
						 ", score=" + std::to_string(mvvLvaScore) + ")");
			}
			if (safe)
			{
				score += 10;
				if (check)
					score += 50;
				if (promotion)
					score += pieceValue(m.promotionType()) * 10;
			}
			else
			{
				if (check)
					score += 30;
				if (promotion)
					score += pieceValue(m.promotionType()) * 5;
			}
			if (!tinfo.threats.empty())
			{
				if (std::find(tinfo.defenseMoves.begin(), tinfo.defenseMoves.end(), m) != tinfo.defenseMoves.end())
				{
					int defenseBonus = tinfo.worst.value * 2;
					score += defenseBonus;
					debugLog("Defense bonus: move " + static_cast<std::string>(m.from()) + " -> " +
							 static_cast<std::string>(m.to()) +
							 " defends against threat, bonus=" + std::to_string(defenseBonus) +
							 ", newScore=" + std::to_string(score));
				}
			}
			int penalty = penalizeIfExposesKing(board, m);
			if (penalty > 0)
			{
				score -= penalty;
				debugLog("Penalty applied: move " +
						 static_cast<std::string>(m.from()) + " -> " +
						 static_cast<std::string>(m.to()) +
						 " exposes defense, penalty=" + std::to_string(penalty) +
						 ", newScore=" + std::to_string(score));
			}
			ScoredMove sm;
			sm.move = m;
			sm.score = score;
			sm.givesCheck = check;
			sm.isPromotion = promotion;
			sm.capturedValue = capturedVal;
			scoredMoves.push_back(sm);
		}
		if (scoredMoves.empty())
			return randomMove();
		std::sort(scoredMoves.begin(), scoredMoves.end(), compareScoredMoves);
		chess::Move chosenMove = scoredMoves.front().move;
		debugLog("Chosen move: " + static_cast<std::string>(chosenMove.from()) +
				 " -> " + static_cast<std::string>(chosenMove.to()) +
				 " (score=" + std::to_string(scoredMoves.front().score) + ")");
		return chosenMove;
	}
	if (difficulty == 5)
	{
		chess::Move chosenMove = get_best_move_from_stockfish(board);
		if (chosenMove != chess::Move::NO_MOVE)
		{
			debugLog("Chosen move: " + static_cast<std::string>(chosenMove.from()) + " -> " + static_cast<std::string>(chosenMove.to()));
			return chosenMove;
		}
		debugLog("Stockfish failed to provide a legal move.");
		return chess::Move(chess::Move::NO_MOVE);
	}
	debugLog("Unknown difficulty level, falling back to random move.");
	return randomMove();
}

struct TerminalSize
{
	int width = 80;
	int height = 24;
};

TerminalSize terminalSize()
{
	TerminalSize size;
	if (isatty(STDOUT_FILENO))
	{
		winsize ws{};
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
		{
			if (ws.ws_col > 0) size.width = ws.ws_col;
			if (ws.ws_row > 0) size.height = ws.ws_row;
		}
	}
	return size;
}

bool supportsAnsi()
{
	const char *term = std::getenv("TERM");
	return isatty(STDOUT_FILENO) && term && std::string(term) != "dumb";
}

bool supportsUnicode()
{
	const char *term = std::getenv("TERM");
	if (!term || std::string(term) == "dumb") return false;
	const char *locale = std::getenv("LC_ALL");
	if (!locale || !*locale) locale = std::getenv("LC_CTYPE");
	if (!locale || !*locale) locale = std::getenv("LANG");
	if (!locale || !*locale) return false;
	std::string value(locale);
	return value.find("UTF-8") != std::string::npos || value.find("utf8") != std::string::npos || value.find("UTF8") != std::string::npos;
}

char asciiPiece(chess::Piece piece)
{
	char symbol = '?';
	if (piece.type() == chess::PieceType::KING) symbol = 'K';
	else if (piece.type() == chess::PieceType::QUEEN) symbol = 'Q';
	else if (piece.type() == chess::PieceType::ROOK) symbol = 'R';
	else if (piece.type() == chess::PieceType::BISHOP) symbol = 'B';
	else if (piece.type() == chess::PieceType::KNIGHT) symbol = 'N';
	else if (piece.type() == chess::PieceType::PAWN) symbol = 'P';
	return piece.color() == chess::Color::WHITE ? symbol : static_cast<char>(std::tolower(symbol));
}

std::string pieceGlyph(chess::Piece piece, bool unicode)
{
	if (!unicode) return std::string(1, asciiPiece(piece));
	// Reverse Unicode glyphs for contrast on a black terminal background:
	// hollow white glyphs otherwise show the black background through their interiors.
	if (piece.color() == chess::Color::WHITE)
	{
		if (piece.type() == chess::PieceType::KING) return "♚";
		if (piece.type() == chess::PieceType::QUEEN) return "♛";
		if (piece.type() == chess::PieceType::ROOK) return "♜";
		if (piece.type() == chess::PieceType::BISHOP) return "♝";
		if (piece.type() == chess::PieceType::KNIGHT) return "♞";
		if (piece.type() == chess::PieceType::PAWN) return "♟";
		return "?";
	}
	if (piece.type() == chess::PieceType::KING) return "♔";
	if (piece.type() == chess::PieceType::QUEEN) return "♕";
	if (piece.type() == chess::PieceType::ROOK) return "♖";
	if (piece.type() == chess::PieceType::BISHOP) return "♗";
	if (piece.type() == chess::PieceType::KNIGHT) return "♘";
	if (piece.type() == chess::PieceType::PAWN) return "♙";
	return "?";
}

void printBoard(const chess::Board &board, const chess::Move *lastMove = nullptr, const std::vector<chess::Square> &highlightSquares = {})
{
	if (supportsAnsi()) std::cout << "\033[2J\033[H";
	debugLog("Printing board" + std::string(lastMove ? " with last move highlight" : ""));
	TerminalSize size = terminalSize();
	bool unicode = supportsUnicode() && size.width >= 28;
	bool compact = size.width < 20;
	bool shortHeight = size.height < 10;
	int boardWidth = compact ? 12 : 20;
	int padding = supportsAnsi() && size.width > boardWidth ? (size.width - boardWidth) / 2 : 0;
	std::string indent(static_cast<size_t>(padding), ' ');
	const std::string lastColor = supportsAnsi() ? "\033[44m" : "";
	const std::string moveColor = supportsAnsi() ? "\033[42m" : "";
	const std::string checkColor = supportsAnsi() ? "\033[41m" : "";
	const std::string reset = supportsAnsi() ? "\033[0m" : "";
	if (!shortHeight) std::cout << "\n" << indent << (compact ? "abcdefgh" : "  a b c d e f g h") << "\n";
	for (int rank = 7; rank >= 0; --rank)
	{
		std::cout << indent << rank + 1 << ' ';
		for (int file = 0; file < 8; ++file)
		{
			chess::Square square(rank * 8 + file);
			chess::Piece piece = board.at(square);
			std::string symbol = piece == chess::Piece::NONE ? ((rank + file) % 2 == 0 ? "." : ":") : pieceGlyph(piece, unicode);
			const std::string *color = nullptr;
			if (lastMove && board.inCheck())
			{
				chess::Bitboard attackers = chess::attacks::attackers(board, ~board.sideToMove(), board.kingSq(board.sideToMove()));
				if (square == board.kingSq(board.sideToMove()) || (attackers & chess::Bitboard::fromSquare(square))) color = &checkColor;
			}
			else if (lastMove && (square == lastMove->from() || square == lastMove->to())) color = &lastColor;
			else if (std::find(highlightSquares.begin(), highlightSquares.end(), square) != highlightSquares.end()) color = &moveColor;
			if (color) std::cout << *color << symbol << reset;
			else std::cout << symbol;
			if (!compact && file != 7) std::cout << ' ';
		}
		std::cout << ' ' << rank + 1 << '\n';
	}
	if (!shortHeight) std::cout << indent << (compact ? "abcdefgh" : "  a b c d e f g h") << "\n";
}

void printHistory(const std::vector<std::string> &history)
{
	debugLog("Printing move history, total moves: " + std::to_string(history.size()));
	std::cout << "\nMove history:\n";
	TerminalSize size = terminalSize();
	size_t visibleMoveRows = static_cast<size_t>(std::max(1, std::min(36, size.height - 20)));
	size_t maxHalfMoves = visibleMoveRows * 2;
	size_t start = history.size() > maxHalfMoves ? history.size() - maxHalfMoves : 0;
	if (start % 2 != 0) --start;
	for (size_t i = start; i < history.size(); ++i)
	{
		if (i % 2 == 0) std::cout << (i / 2 + 1) << ". ";
		std::cout << history[i] << (i % 2 == 0 && i + 1 < history.size() ? "  " : "\n");
	}
	for (size_t i = 0; i < history.size(); i += 2)
	{
		std::string line = std::to_string(i / 2 + 1) + ". " + history[i];
		if (i + 1 < history.size()) line += " " + history[i + 1];
		debugLog(line);
	}
}

void printCaptured(const std::vector<chess::Piece> &capturedPieces)
{
	if (capturedPieces.empty()) return;
	TerminalSize size = terminalSize();
	bool unicode = supportsUnicode() && size.width >= 28;
	std::cout << "Captured: ";
	int column = 9;
	for (const auto &piece : capturedPieces)
	{
		std::string symbol = pieceGlyph(piece, unicode);
		int glyphWidth = unicode ? 2 : 1;
		if (column + glyphWidth + 1 > size.width && column > 9)
		{
			std::cout << "\n         ";
			column = 9;
		}
		std::cout << symbol << ' ';
		column += glyphWidth + 1;
	}
	std::cout << '\n';
}

enum class EndReason { NONE, CHECKMATE, STALEMATE, INSUFFICIENT_MATERIAL, SEVENTY_FIVE_MOVE_RULE, FIVEFOLD_REPETITION };

struct GameStatus
{
	EndReason reason = EndReason::NONE;
	chess::Color winner = chess::Color::NONE;
};

GameStatus gameStatus(const chess::Board &board)
{
	chess::Movelist legal;
	chess::movegen::legalmoves(legal, board);
	if (legal.empty())
		return {board.inCheck() ? EndReason::CHECKMATE : EndReason::STALEMATE,
				board.inCheck() ? ~board.sideToMove() : chess::Color::NONE};
	if (board.isInsufficientMaterial()) return {EndReason::INSUFFICIENT_MATERIAL, chess::Color::NONE};
	// The bundled library exposes the half-move clock and repetition count,
	// so the app can apply the automatic FIDE thresholds without editing its generated header.
	if (board.halfMoveClock() >= 150) return {EndReason::SEVENTY_FIVE_MOVE_RULE, chess::Color::NONE};
	if (board.isRepetition(4)) return {EndReason::FIVEFOLD_REPETITION, chess::Color::NONE};
	return {};
}

void printGameStatus(const GameStatus &status)
{
	switch (status.reason)
	{
	case EndReason::CHECKMATE:
		std::cout << "\n" << (status.winner == chess::Color::WHITE ? "White" : "Black") << " wins by checkmate!\n";
		break;
	case EndReason::STALEMATE: std::cout << "\nGame drawn by stalemate.\n"; break;
	case EndReason::INSUFFICIENT_MATERIAL: std::cout << "\nGame drawn by insufficient material.\n"; break;
	case EndReason::SEVENTY_FIVE_MOVE_RULE: std::cout << "\nGame drawn by the 75-move rule.\n"; break;
	case EndReason::FIVEFOLD_REPETITION: std::cout << "\nGame drawn by fivefold repetition.\n"; break;
	case EndReason::NONE: break;
	}
}

bool runGame()
{
	debugLog("=== New Chess Session Started ===", true);
	chess::Board board;
	board.setFen(chess::constants::STARTPOS);
	std::string gameModeInput;
	std::string DifficultyInput;
	int gameMode = 0;
	int difficulty = 0;
	bool playerIsWhite = true;
	while (true)
	{
		if (supportsAnsi()) std::cout << "\033[2J\033[H";
		std::cout << "Choose game mode:\n";
		std::cout << "1. Player vs Player\n";
		std::cout << "2. Player vs AI\n";
		std::cout << "3. AI vs AI\n";
		std::cout << "Enter choice (1/2/3): ";
		auto gameModeLine = getLineInput();
		if (!gameModeLine) return false;
		gameModeInput = *gameModeLine;
		if (gameModeInput.size() == 1 && (gameModeInput == "1" || gameModeInput == "2" || gameModeInput == "3"))
		{
			gameMode = std::stoi(gameModeInput);
			break;
		}
		else
		{
			std::cout << "Invalid choice. Please enter 1, 2, or 3.\n";
		}
	}
	debugLog("Game mode: " + std::to_string(gameMode));
	if (gameMode == 2)
	{
		while (true)
		{
			if (supportsAnsi()) std::cout << "\033[2J\033[H";
			std::cout << "Choose difficulty:\n";
			std::cout << "1. Novice\n";
			std::cout << "2. Strategist\n";
			std::cout << "3. Master\n";
			std::cout << "4. Human (human-like AI)\n";
			std::cout << "5. Stockfish\n";
			std::cout << "Enter choice (1/2/3/4/5): ";
			auto difficultyLine = getLineInput();
			if (!difficultyLine) return false;
			DifficultyInput = *difficultyLine;
			if (DifficultyInput.size() == 1 && (DifficultyInput == "1" || DifficultyInput == "2" || DifficultyInput == "3" || DifficultyInput == "4" || DifficultyInput == "5"))
			{
				difficulty = std::stoi(DifficultyInput);
				debugLog("Difficulty: " + std::to_string(difficulty));
				break;
			}
			else
			{
				std::cout << "Invalid choice. Please enter 1/2/3/4/5.\n";
			}
		}
		while (true)
		{
			if (supportsAnsi()) std::cout << "\033[2J\033[H";
			std::cout << "Choose side:\n";
			std::cout << "1. Play as White\n";
			std::cout << "2. Play as Black\n";
			std::cout << "Enter choice (1/2): ";
			auto sideLine = getLineInput();
			if (!sideLine) return false;
			std::string sideInput = *sideLine;
			if (sideInput.size() == 1 && (sideInput == "1" || sideInput == "2"))
			{
				playerIsWhite = (sideInput == "1");
				debugLog(std::string("Player chose side: ") + (playerIsWhite ? "White" : "Black"));
				break;
			}
			else
			{
				std::cout << "Invalid choice. Please enter 1 or 2.";
			}
		}
	}
	else if (gameMode == 3)
	{
		difficulty = 5;
	}
	bool stockfishForfeit = false;
	std::vector<std::string> moveHistory;
	std::vector<chess::Piece> moveCaptured;
	chess::Move lastMove;
	printBoard(board);
	while (true)
	{
		debugLog("---- New turn ----");
		if (!moveHistory.empty())
		{
			debugLog("Printing move history");
			printHistory(moveHistory);
		}
		if (!moveCaptured.empty())
		{
			debugLog("Printing Captured history");
			printCaptured(moveCaptured);
		}
		if (gameMode == 1 || gameMode == 2)
		{
			GameStatus status = gameStatus(board);
			debugLog("Checking if game is over");
			if (status.reason != EndReason::NONE)
			{
				debugLog("Game over detected with reason " + std::to_string(static_cast<int>(status.reason)));
				printGameStatus(status);
				break;
			}
			bool aiTurn = (gameMode == 2 && ((board.sideToMove() == chess::Color::WHITE) != playerIsWhite));
			if (aiTurn)
			{
				debugLog(std::string(board.sideToMove() == chess::Color::WHITE ? "White" : "Black") + " AI is starting...");
				goto AI_MOVE_TURN;
			}
			bool whiteToMove = (board.sideToMove() == chess::Color::WHITE);
			debugLog(std::string(whiteToMove ? "White" : "Black") + " to move");
			std::cout << "\n"
					  << (whiteToMove ? "White" : "Black") << " to move.\n";
			if (board.inCheck())
			{
				debugLog(std::string(whiteToMove ? "White" : "Black") + " is in check");
				std::cout << "\n"
						  << (supportsUnicode() ? "⚠️  " : "") << (whiteToMove ? "White" : "Black") << " is in CHECK!\n";
			}
			std::cout << "\nq/Q to resign\nChoose a piece [square, e.g. e2]: ";
			std::string input;
			std::regex squareRegex("^[a-hA-H][1-8]$");
			auto moveInput = getLineInput();
			if (!moveInput) return false;
			input = *moveInput;
			bool isSquare = std::regex_match(input, squareRegex);
			bool isQuit = (input == "q" || input == "Q");
			if (!(isSquare || isQuit))
			{
				debugLog("Invalid move input,Try again...\n");
				std::cout << "Invalid move input,Try again...\n";
				continue;
			}
			std::transform(input.begin(), input.end(), input.begin(), ::tolower);
			debugLog("User entered: " + input);
			if (input == "q" || input == "Q")
			{
				debugLog(std::string(whiteToMove ? "White" : "Black") + " resigns");
				std::cout << (whiteToMove ? "White" : "Black") << " resigns. "
						  << (whiteToMove ? "Black" : "White") << " wins by resignation.\n";
				break;
			}
			if (input.size() != 2)
			{
				std::cout << "Invalid square. Try again.\n";
				continue;
			}
			chess::Square fromSq(input);
			chess::Movelist legal;
			chess::movegen::legalmoves<chess::movegen::MoveGenType::ALL>(legal, board);
			std::vector<chess::Move> fromMoves;
			for (int i = 0; i < legal.size(); ++i)
			{
				if (legal[i].from() == fromSq)
				{
					fromMoves.push_back(legal[i]);
				}
			}
			if (fromMoves.empty())
			{
				std::cout << "\nNo legal moves from " << input << ". Try another square.\n";
				continue;
			}
			std::vector<chess::Square> highlightSquares;
			for (const auto &m : fromMoves)
			{
				highlightSquares.push_back(m.to());
			}
			highlightSquares.push_back(fromSq);
			printBoard(board, nullptr, highlightSquares);
			std::cout << "Legal moves from " << input << ":\n";
			int total = fromMoves.size();
			int columns = std::max(1, (terminalSize().width - 2) / 20);
			int rows = (total + columns - 1) / columns;
			for (int r = 0; r < rows; r++)
			{
				for (int c = 0; c < columns; c++)
				{
					int idx = r * columns + c;
					if (idx < total)
					{
						std::string moveStr = moveDescription(fromMoves[idx]);
						std::cout << std::setw(20) << std::left << (std::to_string(idx + 1) + ". " + moveStr);
					}
				}
				std::cout << "\n";
			}
			int idx = 0;
			std::cout << "Enter move number or destination square; use the move number for promotions. Enter 0 to reselect:\n";
			std::string choice;
			while (true)
			{
				auto choiceInput = getLineInput();
				if (!choiceInput) return false;
				choice = *choiceInput;
				if (choice.size() > 0)
				{
					if (choice != "0")
					{
						if (std::all_of(choice.begin(), choice.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
						{
							int choiceNumber = 0;
							auto parsed = std::from_chars(choice.data(), choice.data() + choice.size(), choiceNumber);
							if (parsed.ec != std::errc{} || parsed.ptr != choice.data() + choice.size() || choiceNumber <= 0)
							{
								std::cout << "Invalid choice. Try again.\n->";
								continue;
							}
							idx = choiceNumber - 1;
							if (idx < 0 || idx >= (int)fromMoves.size())
							{
								std::cout << "Invalid choice. Try again.\n->";
								continue;
							}
							debugLog("User selected move index: " + std::to_string(idx));
							break;
						}
						else if (std::regex_match(choice, squareRegex))
						{
							std::transform(choice.begin(), choice.end(), choice.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
							std::vector<int> matchingMoves;
							for (int move_ind = 0; move_ind < static_cast<int>(fromMoves.size()); ++move_ind)
							{
								if (static_cast<std::string>(fromMoves[move_ind].to()) == choice)
									matchingMoves.push_back(move_ind);
							}
							if (matchingMoves.size() > 1)
							{
								std::cout << "Choose a numbered move to specify the promotion piece.\n->";
								continue;
							}
							if (!matchingMoves.empty())
							{
								idx = matchingMoves.front();
								break;
							}
							std::cout << "That destination is not legal from this piece. Try a listed move.\n->";
							continue;
						}
						else
						{
							std::cout << "Invalid input:Not Move/Digits\n->";
							debugLog("Invalid input for move selection: " + choice);
							continue;
						}
					}
					else
						break;
				}
				debugLog("Invalid move choice,Try again...\n");
				std::cout << "Invalid move choice,Try again...\n->";
			}
			if (choice == "0")
			{
				debugLog("User chose to go back");
				continue;
			}
			chess::Move move = fromMoves[idx];
			debugLog("Move chosen: " + static_cast<std::string>(move.from()) + " -> " + static_cast<std::string>(move.to()));
			chess::Piece captured = capturedPiece(board, move);
			if (captured != chess::Piece::NONE)
			{
				debugLog("Piece Captured: " + static_cast<std::string>(captured));
				moveCaptured.push_back(captured);
			}
			board.makeMove(move);
			debugLog("Move applied to board: " + static_cast<std::string>(move.from()) + " -> " + static_cast<std::string>(move.to()));
			moveHistory.push_back(moveDescription(move));
			lastMove = move;
			printBoard(board, &lastMove);
		}
		AI_MOVE_TURN:
		if (gameMode == 2 || gameMode == 3)
		{
			GameStatus status = gameStatus(board);
			debugLog("Checking if game is over");
			if (status.reason != EndReason::NONE)
			{
				debugLog("Game over detected with reason " + std::to_string(static_cast<int>(status.reason)));
				printGameStatus(status);
				break;
			}
			debugLog("AI to move");
			std::cout << (board.sideToMove() == chess::Color::WHITE ? "White" : "Black") << "-AI to move.\n";
			if (board.inCheck())
			{
				debugLog(" AI is in check");
				std::cout << "\n" << (supportsUnicode() ? "⚠️ " : "") << (board.sideToMove() == chess::Color::WHITE ? "White" : "Black") << "-AI is in CHECK!\n";
			}
			chess::Move computerMove = getComputerMove(board, difficulty, playerIsWhite);
			if (computerMove == chess::Move::NO_MOVE)
			{
				if (difficulty == 5)
				{
					chess::Color forfeitingSide = board.sideToMove();
					chess::Color winner = ~forfeitingSide;
					debugLog("Stockfish forfeits because it failed to provide a legal move.");
					std::cout << "Stockfish (" << (forfeitingSide == chess::Color::WHITE ? "White" : "Black")
							  << ") forfeits after failing to provide a legal move. "
							  << (winner == chess::Color::WHITE ? "White" : "Black") << " wins.\n";
					if (gameMode == 2 && winner == (playerIsWhite ? chess::Color::WHITE : chess::Color::BLACK))
						std::cout << "You win by forfeit.\n";
					std::cout << "Press Enter to return to the game menu.\n";
					stockfishForfeit = true;
				}
				else
				{
					debugLog("Computer produced NO_MOVE despite a non-terminal position.");
					std::cout << (board.sideToMove() == chess::Color::WHITE ? "White" : "Black") << " AI cannot move.\n";
				}
				break;
			}
			chess::Piece captured = capturedPiece(board, computerMove);
			if (captured != chess::Piece::NONE)
			{
				debugLog("Piece Captured: " + static_cast<std::string>(captured));
				moveCaptured.push_back(captured);
			}
			board.makeMove(computerMove);
			debugLog("Computer move applied to board: " + static_cast<std::string>(computerMove.from()) + " -> " + static_cast<std::string>(computerMove.to()));
			moveHistory.push_back(moveDescription(computerMove));
			lastMove = computerMove;
			printBoard(board, &lastMove);
			std::cout << "\n"
					  << (board.sideToMove() == chess::Color::WHITE ? "Black" : "White") << "-AI plays: " << moveDescription(computerMove) << "\n";
		}
	}
	if (!moveHistory.empty())
	{
		debugLog("Printing final move history");
		printHistory(moveHistory);
	}
	debugLog("Game ended");
	if (stockfishForfeit && !getLineInput()) return false;
	return stockfishForfeit;
}

int main()
{
	std::signal(SIGPIPE, SIG_IGN);
	while (runGame())
		;
	return 0;
}

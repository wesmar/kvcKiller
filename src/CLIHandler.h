// CLIHandler.h
// Console mode: argument parsing, help text, and the main CLI dispatcher.
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// Console setup / teardown helpers
// ---------------------------------------------------------------------------

/// Attaches to the parent process's console and redirects stdio to it.
void SetupConsole();

/// Injects a synthetic Enter keypress so the parent shell redraws its prompt
/// automatically after a GUI-subsystem process exits via AttachConsole.
void SendEnterToConsole();

// ---------------------------------------------------------------------------
// Help / usage
// ---------------------------------------------------------------------------

/// Prints the full usage summary to stdout.
void PrintHelp();

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/// Dispatches the CLI command in \p argv[1].
/// Returns the process exit code (0 = success, 1 = error).
int RunCLI(int argc, LPWSTR* argv);

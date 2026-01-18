# Metadata Journaling for a VSFS-like Filesystem

## Project Overview
This project implements **metadata journaling** for a simple filesystem.
It uses a **redo logging** approach to ensure filesystem consistency after
crashes or unexpected shutdowns.

The goal of the project is to guarantee **atomic updates**, **correct recovery**,
and **metadata integrity**.

---

## Key Features
- Append-only redo journal
- Support for multiple transactions
- Atomic commits using COMMIT records
- Crash-safe recovery through install
- Validator-driven correctness checking

---

## Filesystem Layout
- Superblock
- Journal (fixed size)
- Inode bitmap
- Data bitmap
- Inode table
- Data blocks

Block size: 4096 bytes

---

## Commands

### create <filename>
- Reads current filesystem metadata
- Computes updated metadata in memory
- Writes modified metadata blocks to the journal
- Appends a COMMIT record
- Does not update home locations directly

### install
- Scans the journal sequentially
- Applies only committed transactions
- Discards incomplete transactions
- Clears the journal after installation

---

## Project Structure
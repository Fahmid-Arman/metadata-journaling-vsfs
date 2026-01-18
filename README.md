# Metadata Journaling for a VSFS-like Filesystem

## Project Overview
This project implements **metadata journaling** for a VSFS-like filesystem using
a **redo logging** approach. The goal is to ensure filesystem consistency and
correct recovery in the presence of crashes or unexpected shutdowns.

The implementation focuses on **atomic metadata updates**, **multi-transaction
support**, and **safe installation of committed changes**.

---

## Key Features
- Append-only redo journal
- Support for multiple transactions
- Atomic commits using COMMIT records
- Crash-safe recovery through journal installation
- Validator-driven correctness checking

---

## Filesystem Layout
- Superblock
- Journal (fixed size)
- Inode bitmap
- Data bitmap
- Inode table
- Data blocks

**Block size:** 4096 bytes

---

## Supported Commands

### `create <filename>`
- Reads the current filesystem metadata
- Computes required metadata changes in memory
- Appends modified metadata blocks as DATA records to the journal
- Appends a COMMIT record to seal the transaction
- Does not write metadata directly to home locations

### `install`
- Scans the journal sequentially
- Applies only fully committed transactions
- Safely discards incomplete transactions
- Clears the journal after successful installation

---

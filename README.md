# ⚓ Port Management System – CS F372 Operating Systems Assignment

A C-based simulation of a real-time port scheduling system that manages docking, cargo handling, and secure undocking of cargo ships using POSIX Inter-Process Communication (IPC) mechanisms such as message queues and shared memory. Developed as part of the Operating Systems course (CS F372) at BITS Pilani.

## 📌 Features

- Handles regular, emergency, and outgoing ship scheduling
- Allocates docks based on ship categories and crane capacities
- Manages cargo movement using cranes with weight constraints
- Prioritizes emergency ships fairly without preemption
- Secure undocking via solver-assisted radio frequency guessing
- Timestep-based execution for all operations
- Inter-process communication via POSIX message queues and shared memory

## 🧠 Concepts Demonstrated

- Process scheduling and concurrency
- Shared memory and message queues
- Resource allocation under constraints
- Fair scheduling with emergency prioritization
- Real-time system simulation and IPC

## 🛠️ Technologies Used

- **Language:** C (POSIX-compliant)
- **IPC:** System V Message Queues & Shared Memory
- **Platform:** Ubuntu 22.04 / 24.04 LTS
- **Compiler:** GCC

## 📁 Project Structure
├── scheduler.c # Main scheduler logic
├── scheduler.out # Compiled scheduler binary
├── validation.out # Provided binary to test correctness
├── testcase_X/ # Input folder for test case X
│ └── input.txt # Contains dock and solver details

## 🚀 How to Run

### Step 1: Compile

gcc scheduler.c -o scheduler.out


### Step 2: Run in Two Terminals

**Terminal 1** (Validation):

./validation.out X # X is the test case number

**Terminal 2** (Scheduler):

./scheduler.out X # Use the same X


> Ensure that `scheduler.out`, `validation.out`, and the `testcase_X/` folder are in the same parent directory.

## 🧪 Test Case Constraints

- Up to 600 total ships across all types  
- Up to 30 docks and 8 solver processes  
- Strict timestep limits per test case (e.g., ≤291 timesteps for Testcase 4)  
- Program must complete within 6 real-time minutes

## 📝 Sample `input.txt` Format

<shared_memory_key>
<main_msg_queue_key>
<num_solvers>
<solver_queue_key_1>
<solver_queue_key_2>
...
<num_docks>
<dock_1_category> <crane_weights...>
<dock_2_category> <crane_weights...>
...


Each dock info line includes the dock category followed by crane weight capacities.

## 👤 Author

**H.Rakshitha**  
BITS Pilani, Hyderabad Campus

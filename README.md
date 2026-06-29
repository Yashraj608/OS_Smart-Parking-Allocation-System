[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/BxnyiuQB)

## Instructions to Run

Follow these steps in order to compile and run the source code:

1. **Install dependencies:**
   ```bash
   sudo apt install wsl
   sudo apt-get install -y gcc libncurses5-dev
   ```

2. **Prepare the file:**
   Save the `smart_parking.c` file to your local directory.

3. **Compile the program:**
   ```bash
   gcc -Wall -Wextra -o smart_parking smart_parking.c -lpthread -lncurses -lm
   ```

4. **Execute the program:**
   ```bash
   ./smart_parking
   ```

## Documentation

* [Project Proposal](./OS_Project_Proposal.pdf)
* [Project Report](./OS_Project_Report.pdf)

#include <cudaq.h>
#include <iostream>

struct ghz {
  // 加入 __qpu__ 標記，告知編譯器這是 Quantum Kernel
  void operator()(int num_qubits) __qpu__ {
    cudaq::qvector q(num_qubits);
    h(q[0]);
    for (int i = 0; i < num_qubits - 1; i++) {
      x<cudaq::ctrl>(q[i], q[i + 1]);
    }
    mz(q);
  }
};

int main() {
  auto counts = cudaq::sample(ghz{}, 5);
  counts.dump();
  return 0;
}

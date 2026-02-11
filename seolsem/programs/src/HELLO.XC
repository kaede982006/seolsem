int sum_to(int n) {
  int i = 0;
  int acc = 0;
  for (i = 1; i <= n; i = i + 1) {
    acc = acc + i;
  }
  return acc;
}

int main() {
  int v = sum_to(10);
  print("Hello from SeolC Native ABI");
  print("sum_to(10)=");
  print(v);
  return 0;
}

#include <Wire.h>

void setup() {
  Wire.begin(8);                // join i2c bus with address #8
  Wire.onReceive(receiveEvent); // register event
  Serial.begin(9600);           // start serial for output
}

void loop() {
  delay(100);
}

// function that executes whenever data is received from master
// this function is registered as an event, see setup()
void receiveEvent(int bytes) {
  int n = Wire.available();
  int c, res = 0;
  //Serial.println(n);
  while (n--) {
    c = Wire.read(); // receive byte as a character
    res = (res << 8) | (c & 0xFF);
  }
  
  Serial.println(res);         // print the character
}

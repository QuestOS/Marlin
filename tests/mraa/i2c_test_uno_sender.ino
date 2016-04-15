#include <Wire.h>

void setup() {
  Wire.begin(8);                // join i2c bus with address #8
  Wire.onRequest(requestEvent); // register event
  //Wire.onReceive(receiveEvent);
  Serial.begin(9600);
}

void loop() {
  delay(100);
}

// function that executes whenever data is requested by master
// this function is registered as an event, see setup()
void requestEvent() {
  Serial.print("got");
  Wire.write(11); // respond with message of 6 bytes
  // as expected by master
}

/*
void receiveEvent(int bytes) {
    int c = Wire.read(); // receive byte as a character
    Serial.println(c);         // print the character
 
}
*/

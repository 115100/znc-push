## Building

```
git clone http://git.jordanko.ch/eskimo/Igloo-Push.git
cd Igloo-Push
znc-buildmod push.cpp
```

## Usage

`mv push.so ~/.znc/modules`
 
Then on IRC

`/msg *status loadmod --type=user push`

## Configuration

`/msg *push set device1 token`

* Replace device1 with device2-5 for additional devices
* Replace token with your push token from Settings in Igloo

## Testing

`/msg *push send test`
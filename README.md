
This branch adds in functionality to neopixel and dotstar LED's

To install it from an existing Klipper installation.

First stop the klipper service

``
sudo service klipper stop
``

You will then need to add this branch as a remote branch to your installation
``
cd ~/klipper
git remote add mental405 https://github.com/mental405/klipper.git
``

You can verify the remote was added by typing
```
git remote
```

Once the remote has been added, you will need to pull down the the versioning and branches from github so type

````
get fetch --all
````

You should now be able to switch to the led effects branch by checking it out.

```
git checkout mental405/work-led-effects
````

After the branch is checked out, it would probably be a good idea to flash your controller with new MCU code. A minor change was made to neopixel.c but it was enough of a change to require re-flashing things.

If you decide you are done with it, you can swap back to the main branch with

```
git checkout master
```


Additional documentation and examples can be found in the docs folder under LED-Effects.md in the docs folder.
```
~/klipper/docs/LED_Effect.md
```

Welcome to the Klipper project!

[![Klipper](docs/img/klipper-logo-small.png)](https://www.klipper3d.org/)

https://www.klipper3d.org/

Klipper is a 3d-Printer firmware. It combines the power of a general
purpose computer with one or more micro-controllers. See the
[features document](https://www.klipper3d.org/Features.html) for more
information on why you should use Klipper.

To begin using Klipper start by
[installing](https://www.klipper3d.org/Installation.html) it.

Klipper is Free Software. See the [license](COPYING) or read the
[documentation](https://www.klipper3d.org/Overview.html).

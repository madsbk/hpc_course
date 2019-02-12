#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Tell pylint that Axes3D import is required although never explicitly used
from mpl_toolkits.mplot3d import Axes3D  # pylint: disable=W0611
import matplotlib.pyplot as plt
import matplotlib
import h5py
import numpy as np
import argparse


def gfx_init(xm, ym, zm):
    """Init plot"""

    #    plt.ion()
    fig = plt.figure()
    sub = fig.add_subplot(111, projection='3d')
    sub.xm = xm
    sub.ym = ym
    sub.zm = zm
    return sub


def show(sub, frame_id, hdf5_file):
    """Show plot"""

    sun_and_planets_position = '/%d/sun_and_planets_position' % frame_id
    asteroids_position = '/%d/asteroids_position' % frame_id

    sun_and_planets_position = hdf5_file[str(frame_id)][sun_and_planets_position]
    asteroids_position = hdf5_file[asteroids_position]

    # Sun
    sub.clear()
    sub.scatter(
        sun_and_planets_position[0][0],
        sun_and_planets_position[1][0],
        sun_and_planets_position[2][0],
        s=100,
        marker='o',
        c='yellow')
    # Planets
    sub.scatter(
        sun_and_planets_position[0][1:],
        sun_and_planets_position[1][1:],
        sun_and_planets_position[2][1:],
        s=5,
        marker='o',
        c='blue')

    # Asteroids
    sub.scatter(
        asteroids_position[0][1:],
        asteroids_position[1][1:],
        asteroids_position[2][1:],
        s=.1,
        marker='.',
        c='green')

    sub.set_xbound(-sub.xm, sub.xm)
    sub.set_ybound(-sub.ym, sub.ym)
    try:
        sub.set_zbound(-sub.zm, sub.zm)
    except AttributeError:
        print('Warning: correct 3D plots may require matplotlib-1.1 or later')


def main(hdf5_filename):
    P3 = gfx_init(1e18, 1e18, 1e18)

    with h5py.File(hdf5_filename, 'r') as f:
        frames = sorted([int(k) for k in f.keys()])
        for i in frames:
            show(P3, i, f)
            plt.draw()
            plt.pause(0.01)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Visualize the n-body NICE model')
    parser.add_argument(
        'input_hdf5_file',
        type=str,
        help='Path to the hdf5 file to visualize.'
    )
    args = parser.parse_args()
    main(args.input_hdf5_file)

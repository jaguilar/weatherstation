import argparse
from numbers import Number


parser = argparse.ArgumentParser(prog="windvane_adc_levels")
parser.add_argument(
    "--optimize",
    action="store_true",
    help="Finds the best impedance to use for the divider resistor "
    "given the resistances of the weathervane resistors. This "
    "is important on the Pico because the ADC only has ~8 effective bits, "
    "so maximizing the separation between the voltages on each "
    "direction is critical.",
)
parser.add_argument(
    "--impedance",
    help="A number of ohms to use for the divider resistor. "
    "Prints the closest integral ADC level to each switch "
    "direction.",
)
parser.add_argument(
    "--level_bits", default=12, help="The number of bits of level data to show."
)

args = parser.parse_args()


def AdcLevels8Bits(resistors: dict[str, Number], divider_impedance: Number):
    if divider_impedance < 1:
        raise Exception
    ret = {}
    for direction, impedance in resistors.items():
        fraction = divider_impedance / (divider_impedance + impedance)
        oneword = fraction * 2**args.level_bits
        ret[direction] = oneword
    return ret


def MinLevelDiff(levels: dict[str, Number]):
    slevels = sorted(levels.values())
    min_diff = 256
    for i in range(len(slevels) - 1):
        diff = slevels[i + 1] - slevels[i]
        if diff < min_diff:
            min_diff = diff
    return min_diff


if __name__ == "__main__":
    args = parser.parse_args()
    resistors = {
        "NE": 8200,
        "E": 1000,
        "SE": 2200,
        "S": 3900,
        "SW": 16000,
        "W": 120000,
        "NW": 64900,
        "N": 33000,
    }
    combine_resistors = lambda r1, r2: 1 / (1 / resistors[r1] + 1 / resistors[r2])
    resistors["NNE"] = combine_resistors("N", "NE")
    resistors["ENE"] = combine_resistors("NE", "E")
    resistors["ESE"] = combine_resistors("E", "SE")
    resistors["SSE"] = combine_resistors("SE", "S")
    resistors["SSW"] = combine_resistors("S", "SW")
    resistors["WSW"] = combine_resistors("SW", "W")
    resistors["WNW"] = combine_resistors("W", "NW")
    resistors["NNW"] = combine_resistors("NW", "N")

    if args.optimize:
        import scipy.optimize

        result = scipy.optimize.minimize_scalar(
            lambda divider_impedance: -MinLevelDiff(
                AdcLevels8Bits(resistors, divider_impedance)
            ),
            bounds=(100, 200000),
        )
        if not result.success:
            print("Unable to find best impedance for divider: ", str(result))
            exit()
        print(result.x)
    elif args.impedance is not None:
        floatlevels = AdcLevels8Bits(resistors, float(args.impedance))
        intlevels = {k: round(v) for (k, v) in floatlevels.items()}
        print(intlevels)
        print(f"Minimum diff between levels: {MinLevelDiff(floatlevels)}")
    else:
        print("You must specify one of --impedance or --optimize")

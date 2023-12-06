import re

def match_time(line):
    pattern = r'\b\d+\b' 
    match = re.search(pattern, line)
    return (match.group(0))


def main(file = 'dtrace'):
    dtrace_file = open(file, 'r')
    sort_dtrace_file = open('sort_dtrace.py','w')
    lines = dtrace_file.readlines()
    sorta_dict = {}
    for l in lines:
        if 'sync' not in l:
            time = int(match_time(l))
            if time not in sorta_dict.keys():
                sorta_dict[time] = []
            sorta_dict[time].append(l)
        else:
            for key in sorted(sorta_dict):
                for t in sorta_dict[key]:
                    sort_dtrace_file.write(t)
            sorta_dict = {}
            sort_dtrace_file.write(l)  # write sync
    for key in sorted(sorta_dict):
        for t in sorta_dict[key]:
            sort_dtrace_file.write(t)
    sort_dtrace_file.close()


if __name__  == "__main__":
    main()


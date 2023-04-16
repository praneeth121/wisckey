with open('./ExperimentalResults/load_iostats.log', 'r') as f:
    lines = f.readlines()


skip_line = True
total_kb_wrtn = 0
for line in lines:
    if 'nvme0n1' in line:
        if skip_line:
            skip_line = False
        else:
            fields = line.split()
            kb_wrtn = float(fields[6])
            total_kb_wrtn += kb_wrtn

print("Total kB_wrtn for nvme0n1 device during Load Operation:", total_kb_wrtn, "KB")

with open('./ExperimentalResults/aging_iostats.log', 'r') as f:
    lines = f.readlines()

skip_line = True
total_kb_wrtn = 0
for line in lines:
    if 'nvme0n1' in line:
        if skip_line:
            skip_line = False
        else:
            fields = line.split()
            kb_wrtn = float(fields[6])
            total_kb_wrtn += kb_wrtn

print("Total kB_wrtn for nvme0n1 device during Aging Process:", total_kb_wrtn, "KB")
